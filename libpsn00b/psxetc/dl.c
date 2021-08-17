/*
 * PSn00bSDK dynamic linker
 * (C) 2021 spicyjpeg - MPL licensed
 *
 * The bulk of this code is MIPS-specific but not PS1-specific, so the whole
 * dynamic linker could be ported to other MIPS platforms that do not have one
 * in their OS/SDK (e.g. N64, PIC32 microcontrollers). Note that, despite the
 * various ELF references, I did *not* implement a full ELF file parser: this
 * implementation just expects all library files to begin with several metadata
 * sections (.dynamic, .dynsym, .hash, .dynstr) laid out in a fixed order. Use
 * the dll.ld linker script to generate compatible libraries.
 *
 * Most of the stuff implemented here, despite looking like black magic, is
 * actually well-documented across several PDFs scattered around the internet:
 * - http://www.sco.com/developers/devspecs/gabi41.pdf
 * - http://math-atlas.sourceforge.net/devel/assembly/mipsabi32.pdf
 * - http://flint.cs.yale.edu/cs422/doc/ELF_Format.pdf
 *
 * TODO:
 * - Clean up duplicated code, especially hash-table-related loops
 * - Fix bugs and improve allocation checks
 * - Find a way to predict the smallest ELF hash table size for a certain set
 *   of entries
 */

#include <sys/types.h>
#include <stdio.h>
#include <malloc.h>
#include <ctype.h>
#include <elf.h>
#include <dlfcn.h>
#include <string.h>
#include <psxapi.h>

/* Compile options */

// Uncomment before building to enable debug logging (via printf()).
#define DEBUG

// Comment before building to disable functions that rely on BIOS file APIs,
// i.e. DL_LoadSymbolMap() and dlopen().
#define USE_FILE_API

/* Private types */

typedef struct {
	uint32_t hash;
	void     *ptr;
} MapEntry;

typedef enum {
	ERR_NONE,
	ERR_FILE,
	ERR_FILE_MALLOC,
	ERR_FILE_READ,
	ERR_NO_MAP,
	ERR_MAP_MALLOC,
	ERR_NO_SYMBOLS,
	ERR_DLL_NULL,
	ERR_DLL_MALLOC,
	ERR_DLL_FORMAT,
	ERR_NO_FILE_API,
	ERR_MAP_SYMBOL,
	ERR_DLL_SYMBOL
} ErrorCode;

/* Data */

static const char *const DL_ERROR_MESSAGES[] = {
	"Unable to find file",
	"Unable to allocate buffer to load file into",
	"Failed to read file",
	"No symbol map has been loaded yet",
	"Unable to allocate symbol map structures",
	"No symbols found in symbol map",
	"Unable to initialize DLL from null pointer",
	"Unable to allocate DLL metadata structures",
	"Unsupported DLL type or format",
	"psxetc has been built without file support",
	"Symbol not found in symbol map",
	"Symbol not found in DLL"
};

static ErrorCode _error_code       = ERR_NONE;
static uint32_t  *_map_hash_table  = 0;
static MapEntry  *_map_entry_table = 0;

// Accessed by _dl_resolve_helper, stores the pointer to the current resolver
// function. Can be changed using DL_SetResolveCallback().
void *(*_dl_resolve_callback)(DLL *, const char *) = 0;

/* Private utilities */

#ifdef DEBUG
#define _LOG(...) printf(__VA_ARGS__)
#define _ERROR(code, ret) { \
	_LOG("psxetc: ERROR! %s\n", DL_ERROR_MESSAGES[code - 1]); \
	_error_code = code; \
	return ret; \
}
#else
#define _LOG(...)
#define _ERROR(code, ret) { \
	_error_code = code; \
	return ret; \
}
#endif

void _dl_resolve_wrapper(void);

// Called by _dl_resolve_wrapper() (which is in turn called by GCC stubs) to
// resolve a function.
void *_dl_resolve_helper(DLL *dll, uint32_t index) {
	Elf32_Sym  *sym   = &(dll->symtab[index]);
	const char *_name = &(dll->strtab[sym->st_name]);
	void       *address;

	if (_dl_resolve_callback)
		address = _dl_resolve_callback(dll, _name);
	else
		address = DL_GetSymbolByName(_name);

	if (!address) {
		_LOG("psxetc: FATAL! Can't resolve %s, locking up\n", _name);
		while (1)
			__asm__ volatile("nop");
	}

	// Patch the GOT entry to "cache" the resolved address. This can probably
	// be implemented in a faster way, but this thing is already too complex.
	for (uint32_t i = 0; i < dll->got_length; i++) {
		if (dll->got[2 + i] == (uint32_t) sym->st_value) {
			dll->got[2 + i] = (uint32_t) address;
			break;
		}
	}

	return address;
}

// Implementation of the weird obscure hashing function used in the ELF .hash
// section. Not sure how collision-prone this is.
// https://en.wikipedia.org/wiki/PJW_hash_function
static uint32_t _elf_hash(const char *str) {
	uint32_t value = 0;

	while (*str) {
		value <<= 4;
		value  += (uint32_t) *(str++);

		uint32_t nibble = value & 0xf0000000;
		if (nibble)
			value ^= nibble >> 24;

		value &= ~nibble;
	}

	return value;
}

#ifdef USE_FILE_API
static uint8_t *_load_file(const char *filename, size_t *size_output) {
	int32_t fd = open(filename, 1);
	if (fd < 0)
		_ERROR(ERR_FILE, 0);

	// Extract file size from the file's associated control block.
	// https://problemkaputt.de/psx-spx.htm#biosmemorymap
	FCB    *fcb = (FCB *) *((FCB **) 0x80000140);
	size_t size = fcb[fd].filesize;

	uint8_t *buffer = malloc(size);
	if (!buffer)
		_ERROR(ERR_FILE_MALLOC, 0);

	_LOG("psxetc: Loading %s (%d bytes)..", filename, size);

	for (uint32_t offset = 0; offset < size; ) {
		int32_t length = read(fd, &(buffer[offset]), 0x800);

		if (length <= 0) {
			close(fd);
			free(buffer);
			_ERROR(ERR_FILE_READ, 0);
		}

		_LOG(".");
		offset += length;
	}

	close(fd);
	_LOG(" done\n");

	if (size_output)
		*size_output = size;
	return buffer;
}
#endif

/* Symbol map loading/parsing API */

int32_t DL_ParseSymbolMap(const char *ptr, size_t size) {
	DL_UnloadSymbolMap();

	// Perform a quick scan over the entire map text and count the number of
	// newlines. This allows us to (over)estimate the number of entries and
	// allocate a sufficiently large hash/entry table.
	uint32_t entries = 0;
	for (uint32_t pos = 0; pos < size; pos++) {
		if (ptr[pos] == '\n')
			entries++;
	}

	// TODO: find a way to calculate the optimal number of hash table "buckets"
	// in order to minimize hash table size
	uint32_t nbucket = entries;
	_LOG("psxetc: Predicted %d entries, %d hash buckets\n", entries, nbucket);

	// Allocate an entry table to store parsed symbols in, and an associated
	// hash table (same format as .hash section, with 8-byte header).
	_map_hash_table  = malloc(sizeof(uint32_t) * (2 + nbucket + entries));
	_map_entry_table = malloc(sizeof(MapEntry) * entries);
	if (!_map_hash_table || !_map_entry_table)
		_ERROR(ERR_MAP_MALLOC, -1);

	_map_hash_table[0] = nbucket;
	_map_hash_table[1] = entries;
	for (uint32_t i = 0; i < (nbucket + entries); i++)
		_map_hash_table[2 + i] = 0xffffffff;

	// Go again through the symbol map and fill in the hash table.
	uint32_t index = 0;
	for (uint32_t pos = 0; (pos < size) && ptr[pos]; pos++) {
		char     name[64];
		char     type_string[2];
		uint64_t address64;
		size_t   _size;

		// e.g. "main T ffffffff80000000 100 ...\n"
		int32_t parsed = sscanf(
			&(ptr[pos]),
			"%63s %1s %Lx %x",
			name,
			type_string,
			&address64,
			&_size // Optional, unused (yet)
		);

		if (parsed >= 3) {
			// Drop the upper 32 bits of the address (for some reason MIPS nm
			// insists on printing 64-bit addresses... wtf) and normalize the
			// type letter to upper case, then check if the entry is valid and
			// non-null.
			void     *address = (void *) address64;
			char     _type    = toupper(type_string[0]);
			uint32_t hash     = _elf_hash(name);
			uint32_t hash_mod = hash % nbucket;

			if (address && (
				(_type == 'T') || // .text
				(_type == 'R') || // .rodata
				(_type == 'D') || // .data
				(_type == 'B')    // .bss
			)) {
				_LOG(
					"psxetc: Map sym: %08x,%08x [%c %s]\n",
					address,
					_size,
					_type,
					name
				);

				MapEntry *entry = &(_map_entry_table[index]);
				entry->hash     = hash;
				entry->ptr      = address;

				// Append a reference to the entry to the hash table's chain
				// for the current hash_mod. I can't explain this properly.
				uint32_t *hash_entry = &(_map_hash_table[2 + hash_mod]);
				while (*hash_entry != 0xffffffff)
					hash_entry = &(_map_hash_table[2 + nbucket + *hash_entry]);

				*hash_entry = index;
				index++;
			}
		}

		// Skip the line by advancing the pointer until a newline is found. The
		// for loop will then skip the newline itself.
		while ((pos < size) && (ptr[pos] != '\n'))
			pos++;
	}

	_LOG("psxetc: Parsed %d symbols from map\n", entries);
	if (!entries)
		_ERROR(ERR_NO_SYMBOLS, -1);

	return entries;
}

int32_t DL_LoadSymbolMap(const char *filename) {
#ifdef USE_FILE_API
	size_t size;
	char   *ptr = _load_file(filename, &size);
	if (!ptr)
		return -1;

	int32_t entries = DL_ParseSymbolMap(ptr, size);
	free(ptr);

	return entries;
#else
	_ERROR(ERR_NO_FILE_API, -1);
#endif
}

void DL_UnloadSymbolMap(void) {
	if (!_map_hash_table)
		return;

	free(_map_hash_table);
	free(_map_entry_table);
	_map_hash_table = 0;
}

void *DL_GetSymbolByName(const char *name) {
	if (!_map_hash_table)
		_ERROR(ERR_NO_MAP, 0);

	// https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-48031.html
	uint32_t nbucket  = _map_hash_table[0];
	uint32_t hash     = _elf_hash(name);
	uint32_t hash_mod = hash % nbucket;

	// Go through the hash table's chain until the symbol hash matches the one
	// calculated.
	for (uint32_t i = _map_hash_table[2 + hash_mod]; i;) {
		MapEntry *entry = &(_map_entry_table[i]);

		if (hash == entry->hash) {
			_LOG("psxetc: Map lookup [%s = %08x]\n", name, entry->ptr);
			return entry->ptr;
		}

		i = _map_hash_table[2 + nbucket + i];
	}

	_ERROR(ERR_MAP_SYMBOL, 0);
}

void DL_SetResolveCallback(void *(*callback)(DLL *, const char *)) {
	_dl_resolve_callback = callback;
}

/* Library loading and linking API */

DLL *dlinit(void *ptr, size_t size, DL_ResolveMode mode) {
	if (!ptr)
		_ERROR(ERR_DLL_NULL, 0);

	DLL *dll = malloc(sizeof(DLL));
	if (!dll)
		_ERROR(ERR_DLL_MALLOC, 0);

	dll->ptr        = ptr;
	dll->malloc_ptr = 0;
	dll->size       = size;
	_LOG("psxetc: Initializing DLL at %08x\n", ptr);

	// Interpret the key-value pairs in the .dynamic section to obtain info
	// about all the other sections. The pairs are null-terminated, which makes
	// parsing trivial.
	uint32_t local_got_len = 0;
	uint32_t first_got_sym = 0;

	for (Elf32_Dyn *dyn = (Elf32_Dyn *) ptr; dyn->d_tag; dyn++) {
		_LOG("psxetc: .dynamic %08x=%08x ", dyn->d_tag, dyn->d_un.d_val);

		switch (dyn->d_tag) {
			// Offset of .got section
			case DT_PLTGOT:
				_LOG("[PLTGOT]\n");

				dll->got = (void *) (ptr + dyn->d_un.d_val);
				break;

			// Offset of .hash section
			case DT_HASH:
				_LOG("[HASH]\n");

				dll->hash = (void *) (ptr + dyn->d_un.d_val);
				break;

			// Offset of .dynstr (NOT .strtab) section
			case DT_STRTAB:
				_LOG("[STRTAB]\n");

				dll->strtab = (void *) (ptr + dyn->d_un.d_val);
				break;

			// Offset of .dynsym (NOT .symtab) section
			case DT_SYMTAB:
				_LOG("[SYMTAB]\n");

				dll->symtab = (void *) (ptr + dyn->d_un.d_val);
				break;

			// Length of .dynstr section
			//case DT_STRSZ:
				//_LOG("[STRSZ]\n");
				//break;

			// Length of each .dynsym entry
			case DT_SYMENT:
				_LOG("[SYMENT]\n");

				// Only 16-byte symbol table entries are supported.
				if (dyn->d_un.d_val != sizeof(Elf32_Sym)) {
					free(dll);
					_ERROR(ERR_DLL_FORMAT, 0);
				}
				break;

			// MIPS ABI (?) version
			case DT_MIPS_RLD_VERSION:
				_LOG("[MIPS_RLD_VERSION]\n");

				// Versions other than 1 are unsupported (do they even exist?).
				if (dyn->d_un.d_val != 1) {
					free(dll);
					_ERROR(ERR_DLL_FORMAT, 0);
				}
				break;

			// DLL/ABI flags
			case DT_MIPS_FLAGS:
				_LOG("[MIPS_FLAGS]\n");

				// Shortcut pointers (whatever they are) are not supported.
				if (dyn->d_un.d_val & RHF_QUICKSTART) {
					free(dll);
					_ERROR(ERR_DLL_FORMAT, 0);
				}
				break;

			// Number of local (not to resolve) GOT entries
			case DT_MIPS_LOCAL_GOTNO:
				_LOG("[MIPS_LOCAL_GOTNO]\n");

				local_got_len = dyn->d_un.d_val;
				break;

			// Base address DLL was compiled for
			case DT_MIPS_BASE_ADDRESS:
				_LOG("[MIPS_BASE_ADDRESS]\n");

				// Base addresses other than zero are not supported. It would
				// be easy enough to support them, but why?
				if (dyn->d_un.d_val) {
					free(dll);
					_ERROR(ERR_DLL_FORMAT, 0);
				}
				break;

			// Number of symbol table entries
			case DT_MIPS_SYMTABNO:
				_LOG("[MIPS_SYMTABNO]\n");

				dll->symbol_count = dyn->d_un.d_val;
				break;

			// Index of first unresolved symbol table entry
			//case DT_MIPS_UNREFEXTNO:
				//_LOG("[MIPS_UNREFEXTNO]\n");
				//break;

			// Index of first symbol table entry which has a matching GOT entry
			case DT_MIPS_GOTSYM:
				_LOG("[MIPS_GOTSYM]\n");

				first_got_sym = dyn->d_un.d_val;
				break;

			// Number of pages the GOT is split into (does not apply to PS1)
			//case DT_MIPS_HIPAGENO:
				//_LOG("[MIPS_HIPAGENO]\n");
				//break;

			default:
				_LOG("[ignored]\n");
		}
	}

	// Calculate the number of GOT entries (and symbols, if MIPS_SYMTABNO was
	// not found in the .dynamic section).
	//dll->symbol_count = \
		((uint32_t) dll->hash - (uint32_t) dll->symtab) / sizeof(Elf32_Sym);
	//dll->got_length = \
		((uint32_t) ptr + size - (uint32_t) dll->got) / sizeof(uint32_t) - 2;

	dll->got_length = local_got_len + (dll->symbol_count - first_got_sym) - 2;
	_LOG(
		"psxetc: %d symbols, %d GOT entries\n",
		dll->symbol_count,
		dll->got_length
	);

	// Relocate the DLL by adding its base address to all pointers in the GOT
	// except the first two, which are reserved. The first entry in particular
	// is a pointer to the lazy resolver, called by auto-generated stubs when a
	// function is first used. got[1] is normally unused, but here we'll set it
	// to the DLL's metadata struct so we can look that up when resolving
	// functions (see _dl_resolve_wrapper()).
	dll->got[0] = (uint32_t) &_dl_resolve_wrapper;
	dll->got[1] = (uint32_t) dll;

	for (uint32_t i = 0; i < dll->got_length; i++)
		dll->got[2 + i] += (uint32_t) ptr;

	// Fix addresses in the symbol table.
	// TODO: clean this shit up
	uint32_t got_offset = first_got_sym;

	for (uint32_t i = 0; i < dll->symbol_count; i++) {
		Elf32_Sym  *sym   = &(dll->symtab[i]);
		const char *_name = &(dll->strtab[sym->st_name]);

		if (!sym->st_value)
			continue;

		sym->st_value += (uint32_t) ptr;
		_LOG(
			"psxetc: DLL sym: %08x,%08x [%s]\n",
			sym->st_value,
			sym->st_size,
			_name
		);

		// If RTLD_NOW was passed, resolve GOT entries ahead of time by
		// cross-referencing them with the symbol table.
		if (mode != RTLD_NOW)
			continue;

		for (uint32_t j = got_offset; j < dll->got_length; j++) {
			if (dll->got[2 + j] != (uint32_t) sym->st_value)
				continue;

			got_offset = j;

			// If the symbol is undefined (st_shndx = 0) and is either a
			// variable or a function, resolve it immediately.
			// TODO: linking of external variables needs more testing
			if (!(sym->st_shndx) && (
				ELF32_ST_TYPE(sym->st_info) == STT_OBJECT ||
				ELF32_ST_TYPE(sym->st_info) == STT_FUNC
			)) {
				dll->got[2 + j] = (uint32_t) _dl_resolve_callback(dll, _name);

				if (!dll->got[2 + j]) {
					free(dll);
					_ERROR(ERR_MAP_SYMBOL, 0);
				}
			}

			break;
		}
	}

	EnterCriticalSection();
	FlushCache();
	ExitCriticalSection();

	// Call the DLL's global constructors. This is the same thing we'd do in
	// _start() for regular executables, but we have to do it outside of the
	// DLL as there's no _start() or even a defined entry point within the
	// DLL itself.
	const uint32_t *ctor_list = dlsym(dll, "__CTOR_LIST__");
	if (ctor_list) {
		for (uint32_t i = ((uint32_t) ctor_list[0]); i >= 1; i--) {
			void (*ctor)(void) = (void (*)(void)) ctor_list[i];
			DL_CALL(ctor);
		}
	}

	return dll;
}

DLL *dlopen(const char *filename, DL_ResolveMode mode) {
#ifdef USE_FILE_API
	size_t size;
	char   *ptr = _load_file(filename, &size);
	if (!ptr)
		return 0;

	DLL *dll = dlinit(ptr, size, mode);
	if (dll)
		dll->malloc_ptr = dll->ptr;
	else
		free(ptr);

	return dll;
#else
	_ERROR(ERR_NO_FILE_API, 0);
#endif
}

void dlclose(DLL *dll) {
	if (dll == RTLD_DEFAULT)
		return;

	if (dll->ptr) {
		// Call the DLL's global destructors.
		const uint32_t *dtor_list = dlsym(dll, "__DTOR_LIST__");
		if (dtor_list) {
			for (uint32_t i = 0; i < ((uint32_t) dtor_list[0]); i++) {
				void (*dtor)(void) = (void (*)(void)) dtor_list[i + 1];
				DL_CALL(dtor);
			}
		}
	}

	// If the DLL is associated to a buffer allocated by dlopen(), free that
	// buffer.
	if (dll->malloc_ptr)
		free(dll->malloc_ptr);

	free(dll);
}

void *dlsym(DLL *dll, const char *name) {
	if (dll == RTLD_DEFAULT)
		return DL_GetSymbolByName(name);
		//return _dl_resolve_callback(RTLD_DEFAULT, name);

	// https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-48031.html
	const uint32_t *hash_table = dll->hash;
	uint32_t       nbucket     = hash_table[0];
	uint32_t       hash_mod    = _elf_hash(name) % nbucket;

	// Go through the hash table's chain until the symbol name matches the one
	// provided.
	for (uint32_t i = hash_table[2 + hash_mod]; i;) {
		Elf32_Sym  *sym   = &(dll->symtab[i]);
		const char *_name = &(dll->strtab[sym->st_name]);

		if (!strcmp(name, _name)) {
			_LOG("psxetc: DLL lookup [%s = %08x]\n", name, sym->st_value);
			return sym->st_value;
		}

		i = hash_table[2 + nbucket + i];
	}

	_ERROR(ERR_DLL_SYMBOL, 0);
}

const char *const dlerror(void) {
	uint32_t last = _error_code;
	_error_code   = ERR_NONE;

	if (last)
		return DL_ERROR_MESSAGES[last - 1];

	return 0;
}
