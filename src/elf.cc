#include <isymbolprovider.hh>
#include <symbolfactory.hh>
#include <isymbol.hh>
#include <architecturefactory.hh>
#include <utils.hh>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libelf.h>
#include <map>
#include <list>
#include <string>

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <link.h>

using namespace emilpro;

class ElfProvider : public ISymbolProvider
{
public:
	ElfProvider() :
		m_elf(NULL),
		m_listener(NULL),
		m_elfMemory(NULL),
		m_elfIs32Bit(true)
	{
	}

	virtual ~ElfProvider()
	{
	}

	unsigned match(void *data, size_t dataSize)
	{
		if (dataSize < SELFMAG)
			return ISymbolProvider::NO_MATCH;

		if (memcmp(data, ELFMAG, SELFMAG) == 0)
			return ISymbolProvider::PERFECT_MATCH;

		return ISymbolProvider::NO_MATCH;
	}

	bool parse(void *data, size_t dataSize, ISymbolListener *listener)
	{
		if (!(m_elf = elf_memory((char *)data, dataSize)) ) {
				error("elf_begin failed\n");
				return false;
		}

		size_t sz;
		char *raw = elf_getident(m_elf, &sz);

		if (raw && sz > EI_CLASS)
			m_elfIs32Bit = raw[EI_CLASS] == ELFCLASS32;

		// Already coincides with e_machine
		unsigned int arch = ArchitectureFactory::ARCH_UNKNOWN;
		if (m_elfIs32Bit) {
			Elf32_Ehdr *ehdr = elf32_getehdr(m_elf);

			if (ehdr)
				arch = ehdr->e_machine;
		} else {
			Elf64_Ehdr *ehdr = elf64_getehdr(m_elf);

			if (ehdr)
				arch = ehdr->e_machine;
		}

		ArchitectureFactory::instance().provideArchitecture((ArchitectureFactory::Architecture_t)arch);

		m_elfMemory = (uint8_t *)data;
		m_listener = listener;

		return parseOne(listener);
	}

	bool parseOne(ISymbolListener *listener)
	{
		Elf_Scn *scn = NULL;
		size_t shstrndx;

		m_listener = listener;

		if (elf_getshdrstrndx(m_elf, &shstrndx) < 0) {
				error("elf_getshstrndx failed\n");
				return false;
		}

		while ( (scn = elf_nextscn(m_elf, scn)) != NULL )
		{
			Elf_Data *data;
			uint64_t sh_type;
			char *name;

			if (m_elfIs32Bit) {
				Elf32_Shdr *shdr32 = elf32_getshdr(scn);

				sh_type = shdr32->sh_type;
				name = elf_strptr(m_elf, shstrndx, shdr32->sh_name);
			} else {
				Elf64_Shdr *shdr64 = elf64_getshdr(scn);

				sh_type = shdr64->sh_type;
				name = elf_strptr(m_elf, shstrndx, shdr64->sh_name);
			}

			data = elf_getdata(scn, NULL);
			if(!data) {
					error("elf_getdata failed on section %s\n",
							name);
					return false;
			}

			/* Handle symbols */
			if (sh_type == SHT_SYMTAB)
				handleSymtab(scn);
		}

		return true;
	}

private:
	class Segment
	{
	public:
		Segment(ElfW(Addr) paddr, ElfW(Addr) vaddr, size_t size, ElfW(Word) align) :
			m_paddr(paddr), m_vaddr(vaddr), m_align(align), m_size(size)
		{
		}

		ElfW(Addr) m_paddr;
		ElfW(Addr) m_vaddr;
		ElfW(Word) m_align;
		size_t m_size;
	};

	void handleSymtab(Elf_Scn *scn)
	{
		handleSymtabGeneric(scn, ISymbol::LINK_NORMAL);
	}

	void handleSymtabGeneric(Elf_Scn *scn, enum ISymbol::LinkageType symType)
	{
		Elf_Data *data;
		uint8_t *p; // Pointer to the current entry
		uint64_t sh_link;
		size_t sizeOfSymbol;
		typedef std::map<uint64_t, ISymbol *> SymbolsByAddress_t;
		typedef std::list<ISymbol *> SymbolList_t;
		SymbolsByAddress_t symbolsByAddress;
		SymbolList_t fixupSyms;
		int n;

		if (m_elfIs32Bit) {
			Elf32_Shdr *shdr = elf32_getshdr(scn);

			sizeOfSymbol = sizeof(Elf32_Sym);
			sh_link = shdr->sh_link;
		} else {
			Elf64_Shdr *shdr = elf64_getshdr(scn);

			sizeOfSymbol = sizeof(Elf64_Sym);
			sh_link = shdr->sh_link;
		}

		data = elf_getdata(scn, NULL);
		n = data->d_size / sizeOfSymbol;
		p = (uint8_t *)data->d_buf;

		panic_if(n <= 0,
				"Section data too small (%zd) - no symbols\n",
				data->d_size);

		/* Iterate through all symbols */
		for (int i = 0; i < n; i++)
		{
			const char *sym_name;
			unsigned char st_type;
			Elf_Scn *symScn;
			uint64_t addr;
			uint64_t size;
			uint64_t offset = 0;

			enum ISymbol::SymbolType symType = ISymbol::SYM_TEXT;

			if (m_elfIs32Bit) {
				Elf32_Sym *s = (Elf32_Sym *)p;

				symScn = elf_getscn(m_elf, s->st_shndx);
				sym_name = elf_strptr(m_elf, sh_link, s->st_name);
				st_type = ELF32_ST_TYPE(s->st_info);
				addr = s->st_value;
				size = s->st_size;

				if (symScn) {
					Elf32_Shdr *shdr = elf32_getshdr(symScn);

					if (addr >= shdr->sh_addr)
						offset = addr - shdr->sh_addr + shdr->sh_offset;
				}
			} else {
				Elf64_Sym *s = (Elf64_Sym *)p;

				symScn = elf_getscn(m_elf, s->st_shndx);
				sym_name = elf_strptr(m_elf, sh_link, s->st_name);
				st_type = ELF64_ST_TYPE(s->st_info);
				addr = s->st_value;
				size = s->st_size;

				// File offset
				if (symScn) {
					Elf64_Shdr *shdr = elf64_getshdr(symScn);

					if (addr >= shdr->sh_addr)
						offset = addr - shdr->sh_addr + shdr->sh_offset;
				}
			}

			if (st_type == STT_FILE ||
					!symScn) {

				p += sizeOfSymbol;
				continue;
			}

			if (st_type == STT_OBJECT)
				symType = ISymbol::SYM_DATA;

			ISymbol &sym = SymbolFactory::instance().createSymbol(
					ISymbol::LINK_NORMAL,
					symType,
					sym_name,
					(void *)(m_elfMemory + offset),
					addr,
					size);
			symbolsByAddress[addr] = &sym;
			if (size == 0)
				fixupSyms.push_back(&sym);

			p += sizeOfSymbol;
		}

		for (SymbolList_t::iterator it = fixupSyms.begin();
				it != fixupSyms.end();
				++it) {
			ISymbol *cur = *it;
			SymbolsByAddress_t::iterator myIt = symbolsByAddress.find(cur->getAddress());

			if (myIt == symbolsByAddress.end())
				continue;

			SymbolsByAddress_t::iterator nextIt = std::next(myIt);

			if (nextIt == symbolsByAddress.end())
				continue;

			ISymbol *other = nextIt->second;
			cur->setSize(other->getAddress() - cur->getAddress());
		}

		for (SymbolsByAddress_t::iterator it = symbolsByAddress.begin();
				it != symbolsByAddress.end();
				++it) {
			ISymbol *cur = it->second;

			m_listener->onSymbol(*cur);
		}
	}

	Elf *m_elf;
	ISymbolListener *m_listener;
	uint8_t *m_elfMemory;
	bool m_elfIs32Bit;
};


class Registrer
{
public:
	Registrer()
	{
		ElfProvider *elf = new ElfProvider();

		SymbolFactory::instance().registerProvider(elf);
	}
};

static Registrer registrer;
