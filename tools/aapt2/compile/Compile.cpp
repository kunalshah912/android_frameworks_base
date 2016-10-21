/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ConfigDescription.h"
#include "Diagnostics.h"
#include "Flags.h"
#include "ResourceParser.h"
#include "ResourceTable.h"
#include "compile/IdAssigner.h"
#include "compile/InlineXmlFormatParser.h"
#include "compile/Png.h"
#include "compile/PseudolocaleGenerator.h"
#include "compile/XmlIdCollector.h"
#include "flatten/Archive.h"
#include "flatten/XmlFlattener.h"
#include "proto/ProtoSerialize.h"
#include "util/Files.h"
#include "util/Maybe.h"
#include "util/Util.h"
#include "xml/XmlDom.h"
#include "xml/XmlPullParser.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <android-base/errors.h>
#include <android-base/file.h>
#include <dirent.h>
#include <fstream>
#include <string>

using google::protobuf::io::CopyingOutputStreamAdaptor;
using google::protobuf::io::ZeroCopyOutputStream;

namespace aapt {

struct ResourcePathData {
  Source source;
  std::string resourceDir;
  std::string name;
  std::string extension;

  // Original config str. We keep this because when we parse the config, we may
  // add on
  // version qualifiers. We want to preserve the original input so the output is
  // easily
  // computed before hand.
  std::string configStr;
  ConfigDescription config;
};

/**
 * Resource file paths are expected to look like:
 * [--/res/]type[-config]/name
 */
static Maybe<ResourcePathData> extractResourcePathData(const std::string& path,
                                                       std::string* outError) {
  std::vector<std::string> parts = util::split(path, file::sDirSep);
  if (parts.size() < 2) {
    if (outError) *outError = "bad resource path";
    return {};
  }

  std::string& dir = parts[parts.size() - 2];
  StringPiece dirStr = dir;

  StringPiece configStr;
  ConfigDescription config;
  size_t dashPos = dir.find('-');
  if (dashPos != std::string::npos) {
    configStr = dirStr.substr(dashPos + 1, dir.size() - (dashPos + 1));
    if (!ConfigDescription::parse(configStr, &config)) {
      if (outError) {
        std::stringstream errStr;
        errStr << "invalid configuration '" << configStr << "'";
        *outError = errStr.str();
      }
      return {};
    }
    dirStr = dirStr.substr(0, dashPos);
  }

  std::string& filename = parts[parts.size() - 1];
  StringPiece name = filename;
  StringPiece extension;
  size_t dotPos = filename.find('.');
  if (dotPos != std::string::npos) {
    extension = name.substr(dotPos + 1, filename.size() - (dotPos + 1));
    name = name.substr(0, dotPos);
  }

  return ResourcePathData{Source(path),         dirStr.toString(),
                          name.toString(),      extension.toString(),
                          configStr.toString(), config};
}

struct CompileOptions {
  std::string outputPath;
  Maybe<std::string> resDir;
  bool pseudolocalize = false;
  bool legacyMode = false;
  bool verbose = false;
};

static std::string buildIntermediateFilename(const ResourcePathData& data) {
  std::stringstream name;
  name << data.resourceDir;
  if (!data.configStr.empty()) {
    name << "-" << data.configStr;
  }
  name << "_" << data.name;
  if (!data.extension.empty()) {
    name << "." << data.extension;
  }
  name << ".flat";
  return name.str();
}

static bool isHidden(const StringPiece& filename) {
  return util::stringStartsWith(filename, ".");
}

/**
 * Walks the res directory structure, looking for resource files.
 */
static bool loadInputFilesFromDir(IAaptContext* context,
                                  const CompileOptions& options,
                                  std::vector<ResourcePathData>* outPathData) {
  const std::string& rootDir = options.resDir.value();
  std::unique_ptr<DIR, decltype(closedir)*> d(opendir(rootDir.data()),
                                              closedir);
  if (!d) {
    context->getDiagnostics()->error(DiagMessage() << strerror(errno));
    return false;
  }

  while (struct dirent* entry = readdir(d.get())) {
    if (isHidden(entry->d_name)) {
      continue;
    }

    std::string prefixPath = rootDir;
    file::appendPath(&prefixPath, entry->d_name);

    if (file::getFileType(prefixPath) != file::FileType::kDirectory) {
      continue;
    }

    std::unique_ptr<DIR, decltype(closedir)*> subDir(opendir(prefixPath.data()),
                                                     closedir);
    if (!subDir) {
      context->getDiagnostics()->error(DiagMessage() << strerror(errno));
      return false;
    }

    while (struct dirent* leafEntry = readdir(subDir.get())) {
      if (isHidden(leafEntry->d_name)) {
        continue;
      }

      std::string fullPath = prefixPath;
      file::appendPath(&fullPath, leafEntry->d_name);

      std::string errStr;
      Maybe<ResourcePathData> pathData =
          extractResourcePathData(fullPath, &errStr);
      if (!pathData) {
        context->getDiagnostics()->error(DiagMessage() << errStr);
        return false;
      }

      outPathData->push_back(std::move(pathData.value()));
    }
  }
  return true;
}

static bool compileTable(IAaptContext* context, const CompileOptions& options,
                         const ResourcePathData& pathData,
                         IArchiveWriter* writer,
                         const std::string& outputPath) {
  ResourceTable table;
  {
    std::ifstream fin(pathData.source.path, std::ifstream::binary);
    if (!fin) {
      context->getDiagnostics()->error(DiagMessage(pathData.source)
                                       << strerror(errno));
      return false;
    }

    // Parse the values file from XML.
    xml::XmlPullParser xmlParser(fin);

    ResourceParserOptions parserOptions;
    parserOptions.errorOnPositionalArguments = !options.legacyMode;

    // If the filename includes donottranslate, then the default translatable is
    // false.
    parserOptions.translatable =
        pathData.name.find("donottranslate") == std::string::npos;

    ResourceParser resParser(context->getDiagnostics(), &table, pathData.source,
                             pathData.config, parserOptions);
    if (!resParser.parse(&xmlParser)) {
      return false;
    }

    fin.close();
  }

  if (options.pseudolocalize) {
    // Generate pseudo-localized strings (en-XA and ar-XB).
    // These are created as weak symbols, and are only generated from default
    // configuration
    // strings and plurals.
    PseudolocaleGenerator pseudolocaleGenerator;
    if (!pseudolocaleGenerator.consume(context, &table)) {
      return false;
    }
  }

  // Ensure we have the compilation package at least.
  table.createPackage(context->getCompilationPackage());

  // Assign an ID to any package that has resources.
  for (auto& pkg : table.packages) {
    if (!pkg->id) {
      // If no package ID was set while parsing (public identifiers), auto
      // assign an ID.
      pkg->id = context->getPackageId();
    }
  }

  // Create the file/zip entry.
  if (!writer->startEntry(outputPath, 0)) {
    context->getDiagnostics()->error(DiagMessage(outputPath)
                                     << "failed to open");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->finishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copyingAdaptor(writer);

    std::unique_ptr<pb::ResourceTable> pbTable = serializeTableToPb(&table);
    if (!pbTable->SerializeToZeroCopyStream(&copyingAdaptor)) {
      context->getDiagnostics()->error(DiagMessage(outputPath)
                                       << "failed to write");
      return false;
    }
  }

  if (!writer->finishEntry()) {
    context->getDiagnostics()->error(DiagMessage(outputPath)
                                     << "failed to finish entry");
    return false;
  }
  return true;
}

static bool writeHeaderAndBufferToWriter(const StringPiece& outputPath,
                                         const ResourceFile& file,
                                         const BigBuffer& buffer,
                                         IArchiveWriter* writer,
                                         IDiagnostics* diag) {
  // Start the entry so we can write the header.
  if (!writer->startEntry(outputPath, 0)) {
    diag->error(DiagMessage(outputPath) << "failed to open file");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->finishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copyingAdaptor(writer);
    CompiledFileOutputStream outputStream(&copyingAdaptor);

    // Number of CompiledFiles.
    outputStream.WriteLittleEndian32(1);

    std::unique_ptr<pb::CompiledFile> compiledFile =
        serializeCompiledFileToPb(file);
    outputStream.WriteCompiledFile(compiledFile.get());
    outputStream.WriteData(&buffer);

    if (outputStream.HadError()) {
      diag->error(DiagMessage(outputPath) << "failed to write data");
      return false;
    }
  }

  if (!writer->finishEntry()) {
    diag->error(DiagMessage(outputPath) << "failed to finish writing data");
    return false;
  }
  return true;
}

static bool writeHeaderAndMmapToWriter(const StringPiece& outputPath,
                                       const ResourceFile& file,
                                       const android::FileMap& map,
                                       IArchiveWriter* writer,
                                       IDiagnostics* diag) {
  // Start the entry so we can write the header.
  if (!writer->startEntry(outputPath, 0)) {
    diag->error(DiagMessage(outputPath) << "failed to open file");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->finishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copyingAdaptor(writer);
    CompiledFileOutputStream outputStream(&copyingAdaptor);

    // Number of CompiledFiles.
    outputStream.WriteLittleEndian32(1);

    std::unique_ptr<pb::CompiledFile> compiledFile =
        serializeCompiledFileToPb(file);
    outputStream.WriteCompiledFile(compiledFile.get());
    outputStream.WriteData(map.getDataPtr(), map.getDataLength());

    if (outputStream.HadError()) {
      diag->error(DiagMessage(outputPath) << "failed to write data");
      return false;
    }
  }

  if (!writer->finishEntry()) {
    diag->error(DiagMessage(outputPath) << "failed to finish writing data");
    return false;
  }
  return true;
}

static bool flattenXmlToOutStream(IAaptContext* context,
                                  const StringPiece& outputPath,
                                  xml::XmlResource* xmlRes,
                                  CompiledFileOutputStream* out) {
  BigBuffer buffer(1024);
  XmlFlattenerOptions xmlFlattenerOptions;
  xmlFlattenerOptions.keepRawValues = true;
  XmlFlattener flattener(&buffer, xmlFlattenerOptions);
  if (!flattener.consume(context, xmlRes)) {
    return false;
  }

  std::unique_ptr<pb::CompiledFile> pbCompiledFile =
      serializeCompiledFileToPb(xmlRes->file);
  out->WriteCompiledFile(pbCompiledFile.get());
  out->WriteData(&buffer);

  if (out->HadError()) {
    context->getDiagnostics()->error(DiagMessage(outputPath)
                                     << "failed to write data");
    return false;
  }
  return true;
}

static bool compileXml(IAaptContext* context, const CompileOptions& options,
                       const ResourcePathData& pathData, IArchiveWriter* writer,
                       const std::string& outputPath) {
  if (context->verbose()) {
    context->getDiagnostics()->note(DiagMessage(pathData.source)
                                    << "compiling XML");
  }

  std::unique_ptr<xml::XmlResource> xmlRes;
  {
    std::ifstream fin(pathData.source.path, std::ifstream::binary);
    if (!fin) {
      context->getDiagnostics()->error(DiagMessage(pathData.source)
                                       << strerror(errno));
      return false;
    }

    xmlRes = xml::inflate(&fin, context->getDiagnostics(), pathData.source);

    fin.close();
  }

  if (!xmlRes) {
    return false;
  }

  xmlRes->file.name =
      ResourceName({}, *parseResourceType(pathData.resourceDir), pathData.name);
  xmlRes->file.config = pathData.config;
  xmlRes->file.source = pathData.source;

  // Collect IDs that are defined here.
  XmlIdCollector collector;
  if (!collector.consume(context, xmlRes.get())) {
    return false;
  }

  // Look for and process any <aapt:attr> tags and create sub-documents.
  InlineXmlFormatParser inlineXmlFormatParser;
  if (!inlineXmlFormatParser.consume(context, xmlRes.get())) {
    return false;
  }

  // Start the entry so we can write the header.
  if (!writer->startEntry(outputPath, 0)) {
    context->getDiagnostics()->error(DiagMessage(outputPath)
                                     << "failed to open file");
    return false;
  }

  // Make sure CopyingOutputStreamAdaptor is deleted before we call
  // writer->finishEntry().
  {
    // Wrap our IArchiveWriter with an adaptor that implements the
    // ZeroCopyOutputStream
    // interface.
    CopyingOutputStreamAdaptor copyingAdaptor(writer);
    CompiledFileOutputStream outputStream(&copyingAdaptor);

    std::vector<std::unique_ptr<xml::XmlResource>>& inlineDocuments =
        inlineXmlFormatParser.getExtractedInlineXmlDocuments();

    // Number of CompiledFiles.
    outputStream.WriteLittleEndian32(1 + inlineDocuments.size());

    if (!flattenXmlToOutStream(context, outputPath, xmlRes.get(),
                               &outputStream)) {
      return false;
    }

    for (auto& inlineXmlDoc : inlineDocuments) {
      if (!flattenXmlToOutStream(context, outputPath, inlineXmlDoc.get(),
                                 &outputStream)) {
        return false;
      }
    }
  }

  if (!writer->finishEntry()) {
    context->getDiagnostics()->error(DiagMessage(outputPath)
                                     << "failed to finish writing data");
    return false;
  }
  return true;
}

class BigBufferOutputStream : public io::OutputStream {
 public:
  explicit BigBufferOutputStream(BigBuffer* buffer) : mBuffer(buffer) {}

  bool Next(void** data, int* len) override {
    size_t count;
    *data = mBuffer->nextBlock(&count);
    *len = static_cast<int>(count);
    return true;
  }

  void BackUp(int count) override { mBuffer->backUp(count); }

  int64_t ByteCount() const override { return mBuffer->size(); }

  bool HadError() const override { return false; }

 private:
  BigBuffer* mBuffer;

  DISALLOW_COPY_AND_ASSIGN(BigBufferOutputStream);
};

static bool compilePng(IAaptContext* context, const CompileOptions& options,
                       const ResourcePathData& pathData, IArchiveWriter* writer,
                       const std::string& outputPath) {
  if (context->verbose()) {
    context->getDiagnostics()->note(DiagMessage(pathData.source)
                                    << "compiling PNG");
  }

  BigBuffer buffer(4096);
  ResourceFile resFile;
  resFile.name =
      ResourceName({}, *parseResourceType(pathData.resourceDir), pathData.name);
  resFile.config = pathData.config;
  resFile.source = pathData.source;

  {
    std::string content;
    if (!android::base::ReadFileToString(pathData.source.path, &content)) {
      context->getDiagnostics()->error(
          DiagMessage(pathData.source)
          << android::base::SystemErrorCodeToString(errno));
      return false;
    }

    BigBuffer crunchedPngBuffer(4096);
    BigBufferOutputStream crunchedPngBufferOut(&crunchedPngBuffer);

    // Ensure that we only keep the chunks we care about if we end up
    // using the original PNG instead of the crunched one.
    PngChunkFilter pngChunkFilter(content);
    std::unique_ptr<Image> image = readPng(context, &pngChunkFilter);
    if (!image) {
      return false;
    }

    std::unique_ptr<NinePatch> ninePatch;
    if (pathData.extension == "9.png") {
      std::string err;
      ninePatch = NinePatch::create(image->rows.get(), image->width,
                                    image->height, &err);
      if (!ninePatch) {
        context->getDiagnostics()->error(DiagMessage() << err);
        return false;
      }

      // Remove the 1px border around the NinePatch.
      // Basically the row array is shifted up by 1, and the length is treated
      // as height - 2.
      // For each row, shift the array to the left by 1, and treat the length as
      // width - 2.
      image->width -= 2;
      image->height -= 2;
      memmove(image->rows.get(), image->rows.get() + 1,
              image->height * sizeof(uint8_t**));
      for (int32_t h = 0; h < image->height; h++) {
        memmove(image->rows[h], image->rows[h] + 4, image->width * 4);
      }

      if (context->verbose()) {
        context->getDiagnostics()->note(DiagMessage(pathData.source)
                                        << "9-patch: " << *ninePatch);
      }
    }

    // Write the crunched PNG.
    if (!writePng(context, image.get(), ninePatch.get(), &crunchedPngBufferOut,
                  {})) {
      return false;
    }

    if (ninePatch != nullptr ||
        crunchedPngBufferOut.ByteCount() <= pngChunkFilter.ByteCount()) {
      // No matter what, we must use the re-encoded PNG, even if it is larger.
      // 9-patch images must be re-encoded since their borders are stripped.
      buffer.appendBuffer(std::move(crunchedPngBuffer));
    } else {
      // The re-encoded PNG is larger than the original, and there is
      // no mandatory transformation. Use the original.
      if (context->verbose()) {
        context->getDiagnostics()->note(
            DiagMessage(pathData.source)
            << "original PNG is smaller than crunched PNG"
            << ", using original");
      }

      PngChunkFilter pngChunkFilterAgain(content);
      BigBuffer filteredPngBuffer(4096);
      BigBufferOutputStream filteredPngBufferOut(&filteredPngBuffer);
      io::copy(&filteredPngBufferOut, &pngChunkFilterAgain);
      buffer.appendBuffer(std::move(filteredPngBuffer));
    }

    if (context->verbose()) {
      // For debugging only, use the legacy PNG cruncher and compare the
      // resulting file sizes.
      // This will help catch exotic cases where the new code may generate
      // larger PNGs.
      std::stringstream legacyStream(content);
      BigBuffer legacyBuffer(4096);
      Png png(context->getDiagnostics());
      if (!png.process(pathData.source, &legacyStream, &legacyBuffer, {})) {
        return false;
      }

      context->getDiagnostics()->note(DiagMessage(pathData.source)
                                      << "legacy=" << legacyBuffer.size()
                                      << " new=" << buffer.size());
    }
  }

  if (!writeHeaderAndBufferToWriter(outputPath, resFile, buffer, writer,
                                    context->getDiagnostics())) {
    return false;
  }
  return true;
}

static bool compileFile(IAaptContext* context, const CompileOptions& options,
                        const ResourcePathData& pathData,
                        IArchiveWriter* writer, const std::string& outputPath) {
  if (context->verbose()) {
    context->getDiagnostics()->note(DiagMessage(pathData.source)
                                    << "compiling file");
  }

  BigBuffer buffer(256);
  ResourceFile resFile;
  resFile.name =
      ResourceName({}, *parseResourceType(pathData.resourceDir), pathData.name);
  resFile.config = pathData.config;
  resFile.source = pathData.source;

  std::string errorStr;
  Maybe<android::FileMap> f = file::mmapPath(pathData.source.path, &errorStr);
  if (!f) {
    context->getDiagnostics()->error(DiagMessage(pathData.source) << errorStr);
    return false;
  }

  if (!writeHeaderAndMmapToWriter(outputPath, resFile, f.value(), writer,
                                  context->getDiagnostics())) {
    return false;
  }
  return true;
}

class CompileContext : public IAaptContext {
 public:
  void setVerbose(bool val) { mVerbose = val; }

  bool verbose() override { return mVerbose; }

  IDiagnostics* getDiagnostics() override { return &mDiagnostics; }

  NameMangler* getNameMangler() override {
    abort();
    return nullptr;
  }

  const std::string& getCompilationPackage() override {
    static std::string empty;
    return empty;
  }

  uint8_t getPackageId() override { return 0x0; }

  SymbolTable* getExternalSymbols() override {
    abort();
    return nullptr;
  }

  int getMinSdkVersion() override { return 0; }

 private:
  StdErrDiagnostics mDiagnostics;
  bool mVerbose = false;
};

/**
 * Entry point for compilation phase. Parses arguments and dispatches to the
 * correct steps.
 */
int compile(const std::vector<StringPiece>& args) {
  CompileContext context;
  CompileOptions options;

  bool verbose = false;
  Flags flags =
      Flags()
          .requiredFlag("-o", "Output path", &options.outputPath)
          .optionalFlag("--dir", "Directory to scan for resources",
                        &options.resDir)
          .optionalSwitch("--pseudo-localize",
                          "Generate resources for pseudo-locales "
                          "(en-XA and ar-XB)",
                          &options.pseudolocalize)
          .optionalSwitch(
              "--legacy",
              "Treat errors that used to be valid in AAPT as warnings",
              &options.legacyMode)
          .optionalSwitch("-v", "Enables verbose logging", &verbose);
  if (!flags.parse("aapt2 compile", args, &std::cerr)) {
    return 1;
  }

  context.setVerbose(verbose);

  std::unique_ptr<IArchiveWriter> archiveWriter;

  std::vector<ResourcePathData> inputData;
  if (options.resDir) {
    if (!flags.getArgs().empty()) {
      // Can't have both files and a resource directory.
      context.getDiagnostics()->error(DiagMessage()
                                      << "files given but --dir specified");
      flags.usage("aapt2 compile", &std::cerr);
      return 1;
    }

    if (!loadInputFilesFromDir(&context, options, &inputData)) {
      return 1;
    }

    archiveWriter = createZipFileArchiveWriter(context.getDiagnostics(),
                                               options.outputPath);

  } else {
    inputData.reserve(flags.getArgs().size());

    // Collect data from the path for each input file.
    for (const std::string& arg : flags.getArgs()) {
      std::string errorStr;
      if (Maybe<ResourcePathData> pathData =
              extractResourcePathData(arg, &errorStr)) {
        inputData.push_back(std::move(pathData.value()));
      } else {
        context.getDiagnostics()->error(DiagMessage() << errorStr << " (" << arg
                                                      << ")");
        return 1;
      }
    }

    archiveWriter = createDirectoryArchiveWriter(context.getDiagnostics(),
                                                 options.outputPath);
  }

  if (!archiveWriter) {
    return 1;
  }

  bool error = false;
  for (ResourcePathData& pathData : inputData) {
    if (options.verbose) {
      context.getDiagnostics()->note(DiagMessage(pathData.source)
                                     << "processing");
    }

    if (pathData.resourceDir == "values") {
      // Overwrite the extension.
      pathData.extension = "arsc";

      const std::string outputFilename = buildIntermediateFilename(pathData);
      if (!compileTable(&context, options, pathData, archiveWriter.get(),
                        outputFilename)) {
        error = true;
      }

    } else {
      const std::string outputFilename = buildIntermediateFilename(pathData);
      if (const ResourceType* type = parseResourceType(pathData.resourceDir)) {
        if (*type != ResourceType::kRaw) {
          if (pathData.extension == "xml") {
            if (!compileXml(&context, options, pathData, archiveWriter.get(),
                            outputFilename)) {
              error = true;
            }
          } else if (pathData.extension == "png" ||
                     pathData.extension == "9.png") {
            if (!compilePng(&context, options, pathData, archiveWriter.get(),
                            outputFilename)) {
              error = true;
            }
          } else {
            if (!compileFile(&context, options, pathData, archiveWriter.get(),
                             outputFilename)) {
              error = true;
            }
          }
        } else {
          if (!compileFile(&context, options, pathData, archiveWriter.get(),
                           outputFilename)) {
            error = true;
          }
        }
      } else {
        context.getDiagnostics()->error(
            DiagMessage() << "invalid file path '" << pathData.source << "'");
        error = true;
      }
    }
  }

  if (error) {
    return 1;
  }
  return 0;
}

}  // namespace aapt
