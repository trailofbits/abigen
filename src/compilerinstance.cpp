/*
 * Copyright (c) 2018-present, Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "compilerinstance.h"
#include <iostream>
#include "std_filesystem.h"

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Parse/ParseAST.h>

namespace {
#if LLVM_MAJOR_VERSION <= 4
const auto kClangFrontendInputKindCxx = clang::IK_CXX;
const auto kClangFrontendInputKindC = clang::IK_C;
#else
const auto kClangFrontendInputKindCxx = clang::InputKind::CXX;
const auto kClangFrontendInputKindC = clang::InputKind::C;
#endif

/// This map is used to dispatch AST events to the right callback
using ASTCallbackMap =
    std::unordered_map<CompilerInstance::ASTCallbackType, ASTCallback>;

/// The ASTVisitor class is used to traverse the AST and notify the user code
/// about AST events
class ASTVisitor final : public clang::RecursiveASTVisitor<ASTVisitor> {
  /// The AST context, as received by the clang compiler instance
  clang::ASTContext &ast_context;

  /// The source manager, used to obtain the location (SourceLocation) of each
  /// declaration
  clang::SourceManager &source_manager;

  /// The user callbacks
  ASTCallbackMap &ast_callback_map;

  /// The user-defined callback parameter
  void *callback_parameter{nullptr};

  /// Dispatches an event to the right user callback
  bool dispatchEvent(CompilerInstance::ASTCallbackType type,
                     clang::Decl *declaration) {
    auto it = ast_callback_map.find(type);
    if (it == ast_callback_map.end()) {
      return true;
    }

    auto callback = it->second;
    if (callback == nullptr) {
      return true;
    }

    return callback(declaration, ast_context, source_manager,
                    callback_parameter);
  }

 public:
  /// Constructor
  ASTVisitor(clang::ASTContext &ast_context,
             clang::SourceManager &source_manager,
             ASTCallbackMap &ast_callback_map, void *callback_parameter)
      : ast_context(ast_context),
        source_manager(source_manager),
        ast_callback_map(ast_callback_map),
        callback_parameter{callback_parameter} {}

  /// Destructor
  virtual ~ASTVisitor() = default;

  /// Called each time a function declaration is found
  virtual bool VisitFunctionDecl(clang::FunctionDecl *declaration) {
    return dispatchEvent(CompilerInstance::ASTCallbackType::Function,
                         declaration);
  }
};

/// The ASTConsumer is used to instantiate the ASTVisitor for each translation
/// unit
class ASTConsumer final : public clang::ASTConsumer {
  /// The source manager, used to obtain the location (SourceLocation) of each
  /// declaration
  clang::SourceManager &source_manager;

  /// The user callbacks
  ASTCallbackMap &ast_callback_map;

  /// The user-defined callback parameter
  void *callback_parameter{nullptr};

 public:
  ASTConsumer(clang::SourceManager &source_manager,
              ASTCallbackMap &ast_callback_map, void *callback_parameter)
      : source_manager(source_manager),
        ast_callback_map(ast_callback_map),
        callback_parameter(callback_parameter){};
  virtual ~ASTConsumer() = default;

  virtual void HandleTranslationUnit(clang::ASTContext &ast_context) override {
    ASTVisitor recursive_visitor(ast_context, source_manager, ast_callback_map,
                                 callback_parameter);

    recursive_visitor.TraverseDecl(ast_context.getTranslationUnitDecl());
  }
};

CompilerInstance::Status createClangCompilerInstance(
    std::unique_ptr<clang::CompilerInstance> &compiler,
    const CompilerInstanceSettings &settings, ASTCallbackMap &ast_callback_map,
    void *callback_parameter) {
  compiler.reset();

  std::unique_ptr<clang::CompilerInstance> obj;

  try {
    obj = std::make_unique<clang::CompilerInstance>();
  } catch (const std::bad_alloc &) {
    return CompilerInstance::Status(
        false, CompilerInstance::StatusCode::MemoryAllocationFailure);
  }

  auto &header_search_options = obj->getHeaderSearchOpts();
  header_search_options.UseBuiltinIncludes = 0;
  header_search_options.UseStandardSystemIncludes = 0;
  header_search_options.UseStandardCXXIncludes = 0;
  header_search_options.ResourceDir = settings.profile.resource_dir;

  if (settings.language != Language::C && settings.language != Language::CXX) {
    return CompilerInstance::Status(
        false, CompilerInstance::StatusCode::InvalidLanguage);
  }

  stdfs::path profile_root(settings.profile.root_path);

  auto path_list_it = settings.profile.internal_isystem.find(settings.language);
  if (path_list_it != settings.profile.internal_isystem.end()) {
    const auto &path_list = path_list_it->second;

    for (const auto &path : path_list) {
      std::string absolute_path = (profile_root / path).string();
      header_search_options.AddPath(absolute_path,
                                    clang::frontend::IncludeDirGroup::System,
                                    false, false);
    }
  }

  path_list_it =
      settings.profile.internal_externc_isystem.find(settings.language);
  if (path_list_it != settings.profile.internal_externc_isystem.end()) {
    const auto &path_list = path_list_it->second;

    for (const auto &path : path_list) {
      std::string absolute_path = (profile_root / path).string();
      header_search_options.AddPath(
          absolute_path, clang::frontend::IncludeDirGroup::ExternCSystem, false,
          false);
    }
  }

  for (const auto &path : settings.additional_include_folders) {
    std::error_code err = {};
    auto absolute_path = stdfs::absolute(path, err);

    if (!err) {
      header_search_options.AddPath(absolute_path.string(),
                                    clang::frontend::IncludeDirGroup::System,
                                    false, false);
    }
  }

  clang::LangOptions &language_options = obj->getLangOpts();
  clang::InputKind input_kind;
  clang::LangStandard::Kind language_standard;

  if (settings.language == Language::CXX) {
    language_options.CPlusPlus = 1;
    language_options.RTTI = 1;
    language_options.CXXExceptions = 1;

    input_kind = kClangFrontendInputKindCxx;

    switch (settings.language_standard) {
      case 98:
        language_standard = clang::LangStandard::lang_cxx98;
        break;

      case 11:
        language_standard = clang::LangStandard::lang_cxx11;
        break;

      case 14:
        language_standard = clang::LangStandard::lang_cxx14;
        break;

      default:
        return CompilerInstance::Status(
            false, CompilerInstance::StatusCode::InvalidLanguageStandard);
    }

  } else {
    language_options.CPlusPlus = 0;
    language_options.RTTI = 0;
    language_options.CXXExceptions = 0;

    input_kind = kClangFrontendInputKindC;

    switch (settings.language_standard) {
      case 89:
        language_standard = clang::LangStandard::lang_c89;
        break;

      case 94:
        language_standard = clang::LangStandard::lang_c94;
        break;

      case 99:
        language_standard = clang::LangStandard::lang_c99;
        break;

      case 11:
        language_standard = clang::LangStandard::lang_c11;
        break;

      default:
        return CompilerInstance::Status(
            false, CompilerInstance::StatusCode::InvalidLanguageStandard);
    }
  }

  language_options.GNUMode = settings.enable_gnu_extensions ? 1 : 0;
  language_options.GNUKeywords = 1;
  language_options.Bool = 1;

  auto &invocation = obj->getInvocation();
  invocation.setLangDefaults(language_options, input_kind,
                             llvm::Triple(llvm::sys::getDefaultTargetTriple()),
                             obj->getPreprocessorOpts(), language_standard);

  std::shared_ptr<clang::TargetOptions> target_options =
      std::make_shared<clang::TargetOptions>();

  target_options->Triple = llvm::sys::getDefaultTargetTriple();

  obj->createDiagnostics();

  clang::TargetInfo *target_information = clang::TargetInfo::CreateTargetInfo(
      obj->getDiagnostics(), target_options);

  obj->setTarget(target_information);

  obj->createFileManager();
  obj->createSourceManager(obj->getFileManager());

  obj->createPreprocessor(clang::TU_Complete);
  obj->getPreprocessorOpts().UsePredefines = false;

  auto &preprocessor = obj->getPreprocessor();
  preprocessor.getBuiltinInfo().initializeBuiltins(
      preprocessor.getIdentifierTable(), language_options);

  auto &source_manager = obj->getSourceManager();

  obj->setASTConsumer(llvm::make_unique<ASTConsumer>(
      source_manager, ast_callback_map, callback_parameter));

  obj->createASTContext();

  compiler = std::move(obj);
  obj.release();

  return CompilerInstance::Status(true);
}
}  // namespace

/// Private class data
struct CompilerInstance::PrivateData final {
  /// The compiler settings, such as language and include directories
  CompilerInstanceSettings compiler_settings;

  /// The user callback map, used to notify about AST events such as records or
  /// function declarations
  ASTCallbackMap ast_callback_map;

  /// The user-defined data passed to each callback invocation
  void *callback_parameter{nullptr};
};

CompilerInstance::CompilerInstance(const CompilerInstanceSettings &settings)
    : d(new PrivateData) {
  d->compiler_settings = settings;
}

CompilerInstance::Status CompilerInstance::create(
    CompilerInstanceRef &obj, const CompilerInstanceSettings &settings) {
  obj.reset();

  try {
    auto ptr = new CompilerInstance(settings);
    obj.reset(ptr);

    return Status(true);

  } catch (const std::bad_alloc &) {
    return Status(false, StatusCode::MemoryAllocationFailure);

  } catch (const Status &status) {
    return status;
  }
}

CompilerInstance::~CompilerInstance() {}

void CompilerInstance::registerASTCallback(
    CompilerInstance::ASTCallbackType type, ASTCallback callback) {
  d->ast_callback_map[type] = callback;
}

void CompilerInstance::setASTCallbackParameter(void *user_defined) {
  d->callback_parameter = user_defined;
}

CompilerInstance::Status CompilerInstance::processBuffer(
    const std::string &buffer) {
  std::unique_ptr<clang::CompilerInstance> compiler;
  auto status =
      createClangCompilerInstance(compiler, d->compiler_settings,
                                  d->ast_callback_map, d->callback_parameter);
  if (!status.succeeded()) {
    return status;
  }

  auto &source_manager = compiler->getSourceManager();

  clang::FileID file_id =
      source_manager.createFileID(llvm::MemoryBuffer::getMemBuffer(
          llvm::StringRef(buffer), llvm::StringRef("main.cpp")));

  source_manager.setMainFileID(file_id);

  std::string clang_output_buffer;
  llvm::raw_string_ostream clang_output_stream(clang_output_buffer);

  clang::DiagnosticsEngine &diagnostics_engine = compiler->getDiagnostics();

  clang::TextDiagnosticPrinter diagnostic_consumer(
      clang_output_stream, &diagnostics_engine.getDiagnosticOptions());

  diagnostics_engine.setClient(&diagnostic_consumer, false);

  clang::Preprocessor &preprocessor = compiler->getPreprocessor();

  diagnostic_consumer.BeginSourceFile(compiler->getLangOpts(), &preprocessor);

  clang::ParseAST(preprocessor, &compiler->getASTConsumer(),
                  compiler->getASTContext());

  diagnostic_consumer.EndSourceFile();

  if (diagnostic_consumer.getNumErrors() != 0) {
    return Status(false, StatusCode::CompilationError, clang_output_buffer);
  }

  if (diagnostic_consumer.getNumWarnings() != 0) {
    return Status(true, StatusCode::CompilationWarning, clang_output_buffer);
  }

  return Status(true);
}
