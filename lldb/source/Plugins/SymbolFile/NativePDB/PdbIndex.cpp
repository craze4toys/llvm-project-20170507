//===-- PdbIndex.cpp --------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PdbIndex.h"
#include "PdbUtil.h"

#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/ISectionContribVisitor.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"

#include "lldb/Utility/LLDBAssert.h"
#include "lldb/lldb-defines.h"

using namespace lldb_private;
using namespace lldb_private::npdb;
using namespace llvm::codeview;
using namespace llvm::pdb;

PdbIndex::PdbIndex() : m_cus(*this), m_va_to_modi(m_allocator) {}

#define ASSIGN_PTR_OR_RETURN(result_ptr, expr)                                 \
  {                                                                            \
    auto expected_result = expr;                                               \
    if (!expected_result)                                                      \
      return expected_result.takeError();                                      \
    result_ptr = &expected_result.get();                                       \
  }

llvm::Expected<std::unique_ptr<PdbIndex>>
PdbIndex::create(std::unique_ptr<llvm::pdb::PDBFile> file) {
  lldbassert(file);

  std::unique_ptr<PdbIndex> result(new PdbIndex());
  ASSIGN_PTR_OR_RETURN(result->m_dbi, file->getPDBDbiStream());
  ASSIGN_PTR_OR_RETURN(result->m_tpi, file->getPDBTpiStream());
  ASSIGN_PTR_OR_RETURN(result->m_ipi, file->getPDBIpiStream());
  ASSIGN_PTR_OR_RETURN(result->m_info, file->getPDBInfoStream());
  ASSIGN_PTR_OR_RETURN(result->m_publics, file->getPDBPublicsStream());
  ASSIGN_PTR_OR_RETURN(result->m_globals, file->getPDBGlobalsStream());
  ASSIGN_PTR_OR_RETURN(result->m_symrecords, file->getPDBSymbolStream());

  result->m_tpi->buildHashMap();

  result->m_file = std::move(file);

  return std::move(result);
}

lldb::addr_t PdbIndex::MakeVirtualAddress(uint16_t segment,
                                          uint32_t offset) const {
  // Segment indices are 1-based.
  lldbassert(segment > 0);

  uint32_t max_section = dbi().getSectionHeaders().size();
  lldbassert(segment <= max_section + 1);

  // If this is an absolute symbol, it's indicated by the magic section index
  // |max_section+1|.  In this case, the offset is meaningless, so just return.
  if (segment == max_section + 1)
    return LLDB_INVALID_ADDRESS;

  const llvm::object::coff_section &cs = dbi().getSectionHeaders()[segment - 1];
  return m_load_address + static_cast<lldb::addr_t>(cs.VirtualAddress) +
         static_cast<lldb::addr_t>(offset);
}

lldb::addr_t PdbIndex::MakeVirtualAddress(const SegmentOffset &so) const {
  return MakeVirtualAddress(so.segment, so.offset);
}

llvm::Optional<uint16_t>
PdbIndex::GetModuleIndexForAddr(uint16_t segment, uint32_t offset) const {
  return GetModuleIndexForVa(MakeVirtualAddress(segment, offset));
}

llvm::Optional<uint16_t> PdbIndex::GetModuleIndexForVa(lldb::addr_t va) const {
  auto iter = m_va_to_modi.find(va);
  if (iter == m_va_to_modi.end())
    return llvm::None;

  return iter.value();
}

void PdbIndex::ParseSectionContribs() {
  class Visitor : public ISectionContribVisitor {
    PdbIndex &m_ctx;
    llvm::IntervalMap<uint64_t, uint16_t> &m_imap;

  public:
    Visitor(PdbIndex &ctx, llvm::IntervalMap<uint64_t, uint16_t> &imap)
        : m_ctx(ctx), m_imap(imap) {}

    void visit(const SectionContrib &C) override {
      if (C.Size == 0)
        return;

      uint64_t va = m_ctx.MakeVirtualAddress(C.ISect, C.Off);
      uint64_t end = va + C.Size;
      // IntervalMap's start and end represent a closed range, not a half-open
      // range, so we have to subtract 1.
      m_imap.insert(va, end - 1, C.Imod);
    }
    void visit(const SectionContrib2 &C) override { visit(C.Base); }
  };
  Visitor v(*this, m_va_to_modi);
  dbi().visitSectionContributions(v);
}

void PdbIndex::BuildAddrToSymbolMap(CompilandIndexItem &cci) {
  lldbassert(cci.m_symbols_by_va.empty() &&
             "Addr to symbol map is already built!");
  uint16_t modi = cci.m_id.modi;
  const CVSymbolArray &syms = cci.m_debug_stream.getSymbolArray();
  for (auto iter = syms.begin(); iter != syms.end(); ++iter) {
    if (!SymbolHasAddress(*iter))
      continue;

    SegmentOffset so = GetSegmentAndOffset(*iter);
    lldb::addr_t va = MakeVirtualAddress(so);

    // We need to add 4 here to adjust for the codeview debug magic
    // at the beginning of the debug info stream.
    uint32_t sym_offset = iter.offset() + 4;
    PdbCompilandSymId cu_sym_id(modi, sym_offset);

    // If the debug info is incorrect, we could have multiple symbols with the
    // same address.  So use try_emplace instead of insert, and the first one
    // will win.
    auto insert_result =
        cci.m_symbols_by_va.insert(std::make_pair(va, PdbSymUid(cu_sym_id)));
    (void)insert_result;

    // The odds of an error in some function such as GetSegmentAndOffset or
    // MakeVirtualAddress are much higher than the odds of encountering bad
    // debug info, so assert that this item was inserted in the map as opposed
    // to having already been there.
    lldbassert(insert_result.second);
  }
}

std::vector<SymbolAndUid> PdbIndex::FindSymbolsByVa(lldb::addr_t va) {
  std::vector<SymbolAndUid> result;

  llvm::Optional<uint16_t> modi = GetModuleIndexForVa(va);
  if (!modi)
    return result;

  CompilandIndexItem &cci = compilands().GetOrCreateCompiland(*modi);
  if (cci.m_symbols_by_va.empty())
    BuildAddrToSymbolMap(cci);

  // The map is sorted by starting address of the symbol.  So for example
  // we could (in theory) have this situation
  //
  // [------------------]
  //    [----------]
  //      [-----------]
  //          [-------------]
  //            [----]
  //               [-----]
  //             ^ Address we're searching for
  // In order to find this, we use the upper_bound of the key value which would
  // be the first symbol whose starting address is higher than the element we're
  // searching for.

  auto ub = cci.m_symbols_by_va.upper_bound(va);

  for (auto iter = cci.m_symbols_by_va.begin(); iter != ub; ++iter) {
    PdbCompilandSymId cu_sym_id = iter->second.asCompilandSym();
    CVSymbol sym = ReadSymbolRecord(cu_sym_id);

    SegmentOffsetLength sol;
    if (SymbolIsCode(sym))
      sol = GetSegmentOffsetAndLength(sym);
    else
      sol.so = GetSegmentAndOffset(sym);

    lldb::addr_t start = MakeVirtualAddress(sol.so);
    lldb::addr_t end = start + sol.length;
    if (va >= start && va < end)
      result.push_back({std::move(sym), iter->second});
  }

  return result;
}

CVSymbol PdbIndex::ReadSymbolRecord(PdbCompilandSymId cu_sym) const {
  // We need to subtract 4 here to adjust for the codeview debug magic
  // at the beginning of the debug info stream.
  const CompilandIndexItem *cci = compilands().GetCompiland(cu_sym.modi);
  auto iter = cci->m_debug_stream.getSymbolArray().at(cu_sym.offset - 4);
  lldbassert(iter != cci->m_debug_stream.getSymbolArray().end());
  return *iter;
}
