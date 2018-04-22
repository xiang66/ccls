#pragma once

#include "clang_cursor.h"
#include "clang_translation_unit.h"
#include "clang_utils.h"
#include "file_consumer.h"
#include "language.h"
#include "lsp.h"
#include "maybe.h"
#include "nt_string.h"
#include "performance.h"
#include "position.h"
#include "serializer.h"
#include "symbol.h"
#include "utils.h"

#include <assert.h>
#include <stdint.h>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <string_view>
#include <vector>

struct IndexFile;
struct IndexType;
struct IndexFunc;
struct IndexVar;
struct QueryFile;

using RawId = uint32_t;

template <typename T>
struct Id {
  RawId id;

  // Invalid id.
  Id() : id(-1) {}
  explicit Id(RawId id) : id(id) {}
  // Id<T> -> Id<void> or Id<T> -> Id<T> is allowed implicitly.
  template <typename U,
            typename std::enable_if_t<std::is_void_v<T> || std::is_same_v<T, U>,
                                      bool> = false>
  Id(Id<U> o) : id(o.id) {}
  template <
      typename U,
      typename std::enable_if_t<!(std::is_void_v<T> || std::is_same_v<T, U>),
                                bool> = false>
  explicit Id(Id<U> o) : id(o.id) {}

  // Needed for google::dense_hash_map.
  explicit operator RawId() const { return id; }

  bool Valid() const { return id != RawId(-1); }

  bool operator==(const Id& o) const { return id == o.id; }
  bool operator!=(const Id& o) const { return id != o.id; }
  bool operator<(const Id& o) const { return id < o.id; }
};

namespace std {
template <typename T>
struct hash<Id<T>> {
  size_t operator()(const Id<T>& k) const { return hash<RawId>()(k.id); }
};
}  // namespace std

template <typename TVisitor, typename T>
void Reflect(TVisitor& visitor, Id<T>& id) {
  Reflect(visitor, id.id);
}

using IndexFileId = Id<IndexFile>;
using IndexTypeId = Id<IndexType>;
using IndexFuncId = Id<IndexFunc>;
using IndexVarId = Id<IndexVar>;

struct SymbolIdx {
  Id<void> id;
  SymbolKind kind;

  bool operator==(const SymbolIdx& o) const {
    return id == o.id && kind == o.kind;
  }
  bool operator<(const SymbolIdx& o) const {
    return !(id == o.id) ? id < o.id : kind < o.kind;
  }
};
MAKE_REFLECT_STRUCT(SymbolIdx, kind, id);

struct Reference {
  Range range;
  Id<void> id;
  SymbolKind kind;
  Role role;

  bool Valid() const { return range.Valid(); }
  operator SymbolIdx() const { return {id, kind}; }
  std::tuple<Range, Id<void>, SymbolKind, Role> ToTuple() const {
    return std::make_tuple(range, id, kind, role);
  }
  bool operator==(const Reference& o) const { return ToTuple() == o.ToTuple(); }
  bool operator<(const Reference& o) const { return ToTuple() < o.ToTuple(); }
};

// |id,kind| refer to the referenced entity.
struct SymbolRef : Reference {
  SymbolRef() = default;
  SymbolRef(Range range, Id<void> id, SymbolKind kind, Role role)
      : Reference{range, id, kind, role} {}
};

// Represents an occurrence of a variable/type, |id,kind| refer to the lexical
// parent.
struct Use : Reference {
  // |file| is used in Query* but not in Index*
  Id<QueryFile> file;
  Use() = default;
  Use(Range range, Id<void> id, SymbolKind kind, Role role, Id<QueryFile> file)
      : Reference{range, id, kind, role}, file(file) {}
};

void Reflect(Reader& visitor, Reference& value);
void Reflect(Writer& visitor, Reference& value);

struct IndexFamily {
  using FileId = Id<IndexFile>;
  using FuncId = Id<IndexFunc>;
  using TypeId = Id<IndexType>;
  using VarId = Id<IndexVar>;
  using Range = ::Range;
};

template <typename D>
struct NameMixin {
  std::string_view Name(bool qualified) const {
    auto self = static_cast<const D*>(this);
    return qualified ? std::string_view(
                           self->detailed_name.c_str() + self->qual_name_offset,
                           self->short_name_offset - self->qual_name_offset +
                               self->short_name_size)
                     : std::string_view(self->detailed_name.c_str() +
                                            self->short_name_offset,
                                        self->short_name_size);
  }
};

template <typename F>
struct TypeDef : NameMixin<TypeDef<F>> {
  // General metadata.
  std::string detailed_name;
  NtString hover;
  NtString comments;

  // While a class/type can technically have a separate declaration/definition,
  // it doesn't really happen in practice. The declaration never contains
  // comments or insightful information. The user always wants to jump from
  // the declaration to the definition - never the other way around like in
  // functions and (less often) variables.
  //
  // It's also difficult to identify a `class Foo;` statement with the clang
  // indexer API (it's doable using cursor AST traversal), so we don't bother
  // supporting the feature.
  Maybe<Use> spell;
  Maybe<Use> extent;

  // Immediate parent types.
  std::vector<typename F::TypeId> bases;

  // Types, functions, and variables defined in this type.
  std::vector<typename F::TypeId> types;
  std::vector<typename F::FuncId> funcs;
  std::vector<typename F::VarId> vars;

  typename F::FileId file;
  // If set, then this is the same underlying type as the given value (ie, this
  // type comes from a using or typedef statement).
  Maybe<typename F::TypeId> alias_of;

  int16_t qual_name_offset = 0;
  int16_t short_name_offset = 0;
  int16_t short_name_size = 0;
  lsSymbolKind kind = lsSymbolKind::Unknown;

  bool operator==(const TypeDef& o) const {
    return detailed_name == o.detailed_name && spell == o.spell &&
           extent == o.extent && alias_of == o.alias_of && bases == o.bases &&
           types == o.types && funcs == o.funcs && vars == o.vars &&
           kind == o.kind && hover == o.hover && comments == o.comments;
  }
};
template <typename TVisitor, typename Family>
void Reflect(TVisitor& visitor, TypeDef<Family>& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER(detailed_name);
  REFLECT_MEMBER(qual_name_offset);
  REFLECT_MEMBER(short_name_offset);
  REFLECT_MEMBER(short_name_size);
  REFLECT_MEMBER(kind);
  REFLECT_MEMBER(hover);
  REFLECT_MEMBER(comments);
  REFLECT_MEMBER(spell);
  REFLECT_MEMBER(extent);
  REFLECT_MEMBER(file);
  REFLECT_MEMBER(alias_of);
  REFLECT_MEMBER(bases);
  REFLECT_MEMBER(types);
  REFLECT_MEMBER(funcs);
  REFLECT_MEMBER(vars);
  REFLECT_MEMBER_END();
}

struct IndexType {
  using Def = TypeDef<IndexFamily>;

  Usr usr;
  IndexTypeId id;

  Def def;
  std::vector<Use> declarations;

  // Immediate derived types.
  std::vector<IndexTypeId> derived;

  // Declared variables of this type.
  std::vector<IndexVarId> instances;

  // Every usage, useful for things like renames.
  // NOTE: Do not insert directly! Use AddUsage instead.
  std::vector<Use> uses;

  bool operator<(const IndexType& other) const { return id < other.id; }
};

template <typename F>
struct FuncDef : NameMixin<FuncDef<F>> {
  // General metadata.
  std::string detailed_name;
  NtString hover;
  NtString comments;
  Maybe<Use> spell;
  Maybe<Use> extent;

  // Method this method overrides.
  std::vector<typename F::FuncId> bases;

  // Local variables or parameters.
  std::vector<typename F::VarId> vars;

  // Functions that this function calls.
  std::vector<SymbolRef> callees;

  typename F::FileId file;
  // Type which declares this one (ie, it is a method)
  Maybe<typename F::TypeId> declaring_type;
  int16_t qual_name_offset = 0;
  int16_t short_name_offset = 0;
  int16_t short_name_size = 0;
  lsSymbolKind kind = lsSymbolKind::Unknown;
  StorageClass storage = StorageClass::Invalid;

  bool operator==(const FuncDef& o) const {
    return detailed_name == o.detailed_name && spell == o.spell &&
           extent == o.extent && declaring_type == o.declaring_type &&
           bases == o.bases && vars == o.vars && callees == o.callees &&
           kind == o.kind && storage == o.storage && hover == o.hover &&
           comments == o.comments;
  }
};

template <typename TVisitor, typename Family>
void Reflect(TVisitor& visitor, FuncDef<Family>& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER(detailed_name);
  REFLECT_MEMBER(qual_name_offset);
  REFLECT_MEMBER(short_name_offset);
  REFLECT_MEMBER(short_name_size);
  REFLECT_MEMBER(kind);
  REFLECT_MEMBER(storage);
  REFLECT_MEMBER(hover);
  REFLECT_MEMBER(comments);
  REFLECT_MEMBER(spell);
  REFLECT_MEMBER(extent);
  REFLECT_MEMBER(file);
  REFLECT_MEMBER(declaring_type);
  REFLECT_MEMBER(bases);
  REFLECT_MEMBER(vars);
  REFLECT_MEMBER(callees);
  REFLECT_MEMBER_END();
}

struct IndexFunc : NameMixin<IndexFunc> {
  using Def = FuncDef<IndexFamily>;

  Usr usr;
  IndexFuncId id;

  Def def;

  struct Declaration {
    // Range of only the function name.
    Use spell;
    // Location of the parameter names.
    std::vector<Range> param_spellings;
  };

  // Places the function is forward-declared.
  std::vector<Declaration> declarations;

  // Methods which directly override this one.
  std::vector<IndexFuncId> derived;

  // Calls/usages of this function. If the call is coming from outside a
  // function context then the FuncRef will not have an associated id.
  //
  // To get all usages, also include the ranges inside of declarations and
  // def.spell.
  std::vector<Use> uses;

  bool operator<(const IndexFunc& other) const { return id < other.id; }
};
MAKE_REFLECT_STRUCT(IndexFunc::Declaration, spell, param_spellings);

template <typename F>
struct VarDef : NameMixin<VarDef<F>> {
  // General metadata.
  std::string detailed_name;
  NtString hover;
  NtString comments;
  // TODO: definitions should be a list of ranges, since there can be more
  //       than one - when??
  Maybe<Use> spell;
  Maybe<Use> extent;

  typename F::FileId file;
  // Type of the variable.
  Maybe<typename F::TypeId> type;

  // Function/type which declares this one.
  int16_t qual_name_offset = 0;
  int16_t short_name_offset = 0;
  int16_t short_name_size = 0;

  lsSymbolKind kind = lsSymbolKind::Unknown;
  // Note a variable may have instances of both |None| and |Extern|
  // (declaration).
  StorageClass storage = StorageClass::Invalid;

  bool is_local() const { return kind == lsSymbolKind::Variable; }

  bool operator==(const VarDef& o) const {
    return detailed_name == o.detailed_name && spell == o.spell &&
           extent == o.extent && type == o.type && kind == o.kind &&
           storage == o.storage && hover == o.hover && comments == o.comments;
  }
};

template <typename TVisitor, typename Family>
void Reflect(TVisitor& visitor, VarDef<Family>& value) {
  REFLECT_MEMBER_START();
  REFLECT_MEMBER(detailed_name);
  REFLECT_MEMBER(qual_name_offset);
  REFLECT_MEMBER(short_name_offset);
  REFLECT_MEMBER(short_name_size);
  REFLECT_MEMBER(hover);
  REFLECT_MEMBER(comments);
  REFLECT_MEMBER(spell);
  REFLECT_MEMBER(extent);
  REFLECT_MEMBER(file);
  REFLECT_MEMBER(type);
  REFLECT_MEMBER(kind);
  REFLECT_MEMBER(storage);
  REFLECT_MEMBER_END();
}

struct IndexVar {
  using Def = VarDef<IndexFamily>;

  Usr usr;
  IndexVarId id;

  Def def;

  std::vector<Use> declarations;
  std::vector<Use> uses;

  bool operator<(const IndexVar& other) const { return id < other.id; }
};

struct IdCache {
  std::string primary_file;
  std::unordered_map<Usr, IndexTypeId> usr_to_type_id;
  std::unordered_map<Usr, IndexFuncId> usr_to_func_id;
  std::unordered_map<Usr, IndexVarId> usr_to_var_id;
  std::unordered_map<IndexTypeId, Usr> type_id_to_usr;
  std::unordered_map<IndexFuncId, Usr> func_id_to_usr;
  std::unordered_map<IndexVarId, Usr> var_id_to_usr;
};

struct IndexInclude {
  // Line that has the include directive. We don't have complete range
  // information - a line is good enough for clicking.
  int line = 0;
  // Absolute path to the index.
  std::string resolved_path;
};

struct IndexFile {
  IdCache id_cache;

  // For both JSON and MessagePack cache files.
  static const int kMajorVersion;
  // For MessagePack cache files.
  // JSON has good forward compatibility because field addition/deletion do not
  // harm but currently no efforts have been made to make old MessagePack cache
  // files accepted by newer ccls.
  static const int kMinorVersion;

  std::string path;
  std::vector<std::string> args;
  int64_t last_modification_time = 0;
  LanguageId language = LanguageId::Unknown;

  // The path to the translation unit cc file which caused the creation of this
  // IndexFile. When parsing a translation unit we generate many IndexFile
  // instances (ie, each header has a separate one). When the user edits a
  // header we need to lookup the original translation unit and reindex that.
  std::string import_file;

  // Source ranges that were not processed.
  std::vector<Range> skipped_by_preprocessor;

  std::vector<IndexInclude> includes;
  std::vector<std::string> dependencies;
  std::vector<IndexType> types;
  std::vector<IndexFunc> funcs;
  std::vector<IndexVar> vars;

  // Diagnostics found when indexing this file. Not serialized.
  std::vector<lsDiagnostic> diagnostics_;
  // File contents at the time of index. Not serialized.
  std::string file_contents;

  IndexFile(const std::string& path, const std::string& contents);

  IndexTypeId ToTypeId(Usr usr);
  IndexFuncId ToFuncId(Usr usr);
  IndexVarId ToVarId(Usr usr);
  IndexTypeId ToTypeId(const CXCursor& usr);
  IndexFuncId ToFuncId(const CXCursor& usr);
  IndexVarId ToVarId(const CXCursor& usr);
  IndexType* Resolve(IndexTypeId id);
  IndexFunc* Resolve(IndexFuncId id);
  IndexVar* Resolve(IndexVarId id);

  std::string ToString();
};

struct NamespaceHelper {
  std::unordered_map<ClangCursor, std::string>
      container_cursor_to_qualified_name;

  std::tuple<std::string, int16_t, int16_t> QualifiedName(
      const CXIdxContainerInfo* container,
      std::string_view unqualified_name);
};

// |import_file| is the cc file which is what gets passed to clang.
// |desired_index_file| is the (h or cc) file which has actually changed.
// |dependencies| are the existing dependencies of |import_file| if this is a
// reparse.
std::vector<std::unique_ptr<IndexFile>> Parse(
    FileConsumerSharedState* file_consumer_shared,
    std::string file,
    const std::vector<std::string>& args,
    const std::vector<FileContents>& file_contents,
    PerformanceImportFile* perf,
    ClangIndex* index);
std::vector<std::unique_ptr<IndexFile>> ParseWithTu(
    FileConsumerSharedState* file_consumer_shared,
    PerformanceImportFile* perf,
    ClangTranslationUnit* tu,
    ClangIndex* index,
    const std::string& file,
    const std::vector<std::string>& args,
    const std::vector<CXUnsavedFile>& file_contents);

bool ConcatTypeAndName(std::string& type, const std::string& name);

void IndexInit();

// Abstracts away the actual indexing process. Each IIndexer instance is
// per-thread and constructing an instance may be extremely expensive (ie,
// acquire a lock) and should be done as rarely as possible.
struct IIndexer {
  struct TestEntry {
    std::string path;
    int num_indexes = 0;
  };

  static std::unique_ptr<IIndexer> MakeTestIndexer(
      std::initializer_list<TestEntry> entries);

  virtual ~IIndexer() = default;
  virtual std::vector<std::unique_ptr<IndexFile>> Index(
      FileConsumerSharedState* file_consumer_shared,
      std::string file,
      const std::vector<std::string>& args,
      const std::vector<FileContents>& file_contents,
      PerformanceImportFile* perf) = 0;
};

struct ClangIndexer : IIndexer {
  ~ClangIndexer() override = default;

  std::vector<std::unique_ptr<IndexFile>> Index(
      FileConsumerSharedState* file_consumer_shared,
      std::string file,
      const std::vector<std::string>& args,
      const std::vector<FileContents>& file_contents,
      PerformanceImportFile* perf) override {
    return Parse(file_consumer_shared, file, args, file_contents, perf, &index);
  }

  // Note: constructing this acquires a global lock
  ClangIndex index;
};
