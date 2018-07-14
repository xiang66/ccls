#pragma once
#include "position.h"
#include "working_files.h"

#include <clang/Frontend/ASTUnit.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/CrashRecoveryContext.h>

#include <memory>
#include <string>
#include <vector>
#include <stdlib.h>

std::vector<clang::ASTUnit::RemappedFile>
GetRemapped(const WorkingFiles::Snapshot &snapshot);

Range FromCharRange(const clang::SourceManager &SM, const clang::LangOptions &LangOpts,
                    clang::SourceRange R,
                    llvm::sys::fs::UniqueID *UniqueID = nullptr);

Range FromTokenRange(const clang::SourceManager &SM, const clang::LangOptions &LangOpts,
                     clang::SourceRange R,
                     llvm::sys::fs::UniqueID *UniqueID = nullptr);

template <typename Fn>
bool RunSafely(llvm::CrashRecoveryContext &CRC, Fn &&fn) {
  const char *env = getenv("CCLS_CRASH_RECOVERY");
  if (env && strcmp(env, "0") == 0) {
    fn();
    return true;
  }
  return CRC.RunSafely(fn);
}

struct ClangTranslationUnit {
  static std::unique_ptr<ClangTranslationUnit>
  Create(const std::string &filepath, const std::vector<std::string> &arguments,
         const WorkingFiles::Snapshot &snapshot);

  int Reparse(llvm::CrashRecoveryContext &CRC,
              const WorkingFiles::Snapshot &snapshot);

  std::shared_ptr<clang::PCHContainerOperations> PCHCO;
  std::unique_ptr<clang::ASTUnit> Unit;
};
