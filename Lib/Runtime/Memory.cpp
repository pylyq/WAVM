#include "Inline/Assert.h"
#include "Inline/BasicTypes.h"
#include "Inline/Lock.h"
#include "Runtime.h"
#include "RuntimePrivate.h"

using namespace Runtime;

// Global lists of memories; used to query whether an address is reserved by one of them.
static Platform::Mutex memoriesMutex;
static std::vector<MemoryInstance*> memories;

enum
{
	numGuardPages = 1
};

static Uptr getPlatformPagesPerWebAssemblyPageLog2()
{
	errorUnless(Platform::getPageSizeLog2() <= IR::numBytesPerPageLog2);
	return IR::numBytesPerPageLog2 - Platform::getPageSizeLog2();
}

MemoryInstance* Runtime::createMemory(Compartment* compartment, MemoryType type)
{
	MemoryInstance* memory = new MemoryInstance(compartment, type);

	// On a 64-bit runtime, allocate 8GB of address space for the memory.
	// This allows eliding bounds checks on memory accesses, since a 32-bit index + 32-bit offset
	// will always be within the reserved address-space.
	const Uptr pageBytesLog2  = Platform::getPageSizeLog2();
	const Uptr memoryMaxBytes = Uptr(8ull * 1024 * 1024 * 1024);
	const Uptr memoryMaxPages = memoryMaxBytes >> pageBytesLog2;

	memory->baseAddress = Platform::allocateVirtualPages(memoryMaxPages + numGuardPages);
	memory->endOffset   = memoryMaxBytes;
	if(!memory->baseAddress)
	{
		delete memory;
		return nullptr;
	}

	// Grow the memory to the type's minimum size.
	wavmAssert(type.size.min <= UINTPTR_MAX);
	if(growMemory(memory, Uptr(type.size.min)) == -1)
	{
		delete memory;
		return nullptr;
	}

	// Add the memory to the compartment.
	if(compartment)
	{
		Lock<Platform::Mutex> compartmentLock(compartment->mutex);

		if(compartment->memories.size() >= maxMemories)
		{
			delete memory;
			return nullptr;
		}

		memory->id = compartment->memories.size();
		compartment->memories.push_back(memory);
		compartment->runtimeData->memories[memory->id] = memory->baseAddress;
	}

	// Add the memory to the global array.
	{
		Lock<Platform::Mutex> memoriesLock(memoriesMutex);
		memories.push_back(memory);
	}
	return memory;
}

MemoryInstance* Runtime::cloneMemory(MemoryInstance* memory, Compartment* newCompartment)
{
	MemoryInstance* newMemory = createMemory(newCompartment, memory->type);
	const Uptr numPages       = memory->numPages;
	growMemory(newMemory, numPages);
	memcpy(newMemory->baseAddress, memory->baseAddress, numPages * IR::numBytesPerPage);
	return newMemory;
}

void Runtime::MemoryInstance::finalize()
{
	Lock<Platform::Mutex> compartmentLock(compartment->mutex);
	wavmAssert(compartment->memories[id] == this);
	wavmAssert(compartment->runtimeData->memories[id] == baseAddress);
	compartment->memories[id]              = nullptr;
	compartment->runtimeData->memories[id] = nullptr;
}

Runtime::MemoryInstance::~MemoryInstance()
{
	// Decommit all default memory pages.
	if(numPages > 0)
	{
		Platform::decommitVirtualPages(baseAddress,
									   numPages << getPlatformPagesPerWebAssemblyPageLog2());
	}

	// Free the virtual address space.
	const Uptr pageBytesLog2 = Platform::getPageSizeLog2();
	if(endOffset > 0)
	{ Platform::freeVirtualPages(baseAddress, (endOffset >> pageBytesLog2) + numGuardPages); }
	baseAddress = nullptr;

	// Remove the memory from the global array.
	{
		Lock<Platform::Mutex> memoriesLock(memoriesMutex);
		for(Uptr memoryIndex = 0; memoryIndex < memories.size(); ++memoryIndex)
		{
			if(memories[memoryIndex] == this)
			{
				memories.erase(memories.begin() + memoryIndex);
				break;
			}
		}
	}
}

bool Runtime::isAddressOwnedByMemory(U8* address)
{
	// Iterate over all memories and check if the address is within the reserved address space for
	// each.
	Lock<Platform::Mutex> memoriesLock(memoriesMutex);
	for(auto memory : memories)
	{
		U8* startAddress = memory->baseAddress;
		U8* endAddress   = memory->baseAddress + memory->endOffset;
		if(address >= startAddress && address < endAddress) { return true; }
	}
	return false;
}

Uptr Runtime::getMemoryNumPages(MemoryInstance* memory) { return memory->numPages; }
Uptr Runtime::getMemoryMaxPages(MemoryInstance* memory)
{
	wavmAssert(memory->type.size.max <= UINTPTR_MAX);
	return Uptr(memory->type.size.max);
}

Iptr Runtime::growMemory(MemoryInstance* memory, Uptr numNewPages)
{
	const Uptr previousNumPages = memory->numPages;
	if(numNewPages > 0)
	{
		// If the number of pages to grow would cause the memory's size to exceed its maximum,
		// return -1.
		if(numNewPages > memory->type.size.max
		   || memory->numPages > memory->type.size.max - numNewPages)
		{ return -1; }

		// Try to commit the new pages, and return -1 if the commit fails.
		if(!Platform::commitVirtualPages(
			   memory->baseAddress + (memory->numPages << IR::numBytesPerPageLog2),
			   numNewPages << getPlatformPagesPerWebAssemblyPageLog2()))
		{ return -1; }
		memory->numPages += numNewPages;
	}
	return previousNumPages;
}

Iptr Runtime::shrinkMemory(MemoryInstance* memory, Uptr numPagesToShrink)
{
	const Uptr previousNumPages = memory->numPages;
	if(numPagesToShrink > 0)
	{
		// If the number of pages to shrink would cause the memory's size to drop below its minimum,
		// return -1.
		if(numPagesToShrink > memory->numPages
		   || memory->numPages - numPagesToShrink < memory->type.size.min)
		{ return -1; }
		memory->numPages -= numPagesToShrink;

		// Decommit the pages that were shrunk off the end of the memory.
		Platform::decommitVirtualPages(
			memory->baseAddress + (memory->numPages << IR::numBytesPerPageLog2),
			numPagesToShrink << getPlatformPagesPerWebAssemblyPageLog2());
	}
	return previousNumPages;
}

void Runtime::unmapMemoryPages(MemoryInstance* memory, Uptr pageIndex, Uptr numPages)
{
	wavmAssert(pageIndex < memory->numPages);
	wavmAssert(pageIndex + numPages > pageIndex);
	wavmAssert(pageIndex + numPages < memory->numPages);

	// Decommit the pages.
	Platform::decommitVirtualPages(memory->baseAddress + (pageIndex << IR::numBytesPerPageLog2),
								   numPages << getPlatformPagesPerWebAssemblyPageLog2());
}

U8* Runtime::getMemoryBaseAddress(MemoryInstance* memory) { return memory->baseAddress; }

U8* Runtime::getValidatedMemoryOffsetRange(MemoryInstance* memory, Uptr offset, Uptr numBytes)
{
	// Validate that the range [offset..offset+numBytes) is contained by the memory's reserved
	// pages.
	U8* address = memory->baseAddress + Platform::saturateToBounds(offset, memory->endOffset);
	if(!memory || address < memory->baseAddress || address + numBytes < address
	   || address + numBytes > memory->baseAddress + memory->endOffset)
	{ throwException(Exception::accessViolationType, {}); }
	return address;
}
