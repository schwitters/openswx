// SPDX-License-Identifier: MIT
// Copyright (c) 2026 openswx contributors

/// @file document.h
/// @brief Main entry point for parsing SolidWorks® document files.
///
/// This header exposes two types:
///  - @ref openswx::Result "Result\<T\>" — a lightweight value-or-error type.
///  - @ref openswx::SwxDocument "SwxDocument" — parses a SolidWorks file and
///    gives access to its metadata.
///
/// **Supported file formats**
/// | Format                              | Extension(s)              | Status      |
/// |-------------------------------------|---------------------------|-------------|
/// | SolidWorks 2015+ (chunk format)     | .SLDPRT / .SLDASM / .SLDDRW | Supported   |
/// | SolidWorks pre-2015 (OLE2/CFB)      | .SLDPRT / .SLDASM / .SLDDRW | Error       |
/// | 3DExperience (ZIP/OPC)              | .SLDPRT / .SLDASM / .SLDDRW | Error       |
///
/// **Quick start**
/// @code{.cpp}
/// #include <openswx/document.h>
///
/// auto result = openswx::SwxDocument::Open("/path/to/part.SLDPRT");
/// if (!result.ok()) {
///     std::cerr << "Error: " << result.error() << "\n";
///     return 1;
/// }
/// const openswx::Document& doc = result.value().doc();
/// for (const auto& cfg : doc.configurations)
///     std::cout << cfg.name << "\n";
/// @endcode

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>

#include "openswx/types.h"

namespace openswx {

// ─────────────────────────────────────────────────────────────────────────────
// Result<T>
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Lightweight value-or-error type (C++20 stand-in for `std::expected`).
///
/// A `Result<T>` holds either a value of type `T` (success) or a human-readable
/// error message (failure).  The class is `[[nodiscard]]` to prevent silently
/// discarding errors at call sites.
///
/// **Typical usage**
/// @code{.cpp}
/// Result<Foo> r = produce();
/// if (!r.ok()) {
///     handle_error(r.error());
///     return;
/// }
/// use(r.value());          // safe because ok() was checked
/// @endcode
///
/// @tparam T Value type.  Must be default-constructible and move-constructible.
template <typename T>
class [[nodiscard]] Result {
 public:
  /// @brief Creates a successful result wrapping @p value.
  [[nodiscard]] static Result Ok(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  /// @brief Creates a failure result with the given human-readable @p message.
  [[nodiscard]] static Result Err(std::string message) {
    return Result(std::in_place_index<1>, std::move(message));
  }

  /// @brief Returns `true` if this result holds a value, `false` on error.
  [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }

  /// @brief Returns a const reference to the value.
  /// @pre ok() == true.  Behaviour is undefined (throws `std::bad_variant_access`)
  ///      when called on an error result.
  [[nodiscard]] const T& value() const& { return std::get<0>(data_); }

  /// @brief Returns the value by rvalue reference (move out).
  /// @pre ok() == true.  Behaviour is undefined (throws `std::bad_variant_access`)
  ///      when called on an error result.
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

  /// @brief Returns the error message string.
  /// @pre ok() == false.  Behaviour is undefined (throws `std::bad_variant_access`)
  ///      when called on a success result.
  [[nodiscard]] const std::string& error() const { return std::get<1>(data_); }

 private:
  template <std::size_t I, typename... Args>
  explicit Result(std::in_place_index_t<I> tag, Args&&... args)
      : data_(tag, std::forward<Args>(args)...) {}

  std::variant<T, std::string> data_;
};

// ─────────────────────────────────────────────────────────────────────────────
// SwxDocument
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Parses a SolidWorks document file and exposes its metadata.
///
/// `SwxDocument` reads and fully decodes a `.SLDPRT`, `.SLDASM`, or `.SLDDRW`
/// file without requiring a SolidWorks installation or any COM/Windows-only
/// dependency.
///
/// **Supported format:** SolidWorks 2015 and later (modern chunk/stream format).
/// Files saved by SolidWorks 2014 or earlier (OLE2/CFB format) are rejected
/// with an error.  3DExperience (ZIP/OPC) files are also not supported.
///
/// **Thread safety:** Multiple threads may call @ref Open() concurrently on
/// different file paths.  A single `SwxDocument` instance must not be accessed
/// from multiple threads simultaneously.
///
/// **Ownership:** All data returned through @ref doc() and the convenience
/// accessors is owned by the `SwxDocument` object.  Pointers and references
/// obtained from it are valid only for the lifetime of the `SwxDocument`.
class SwxDocument {
 public:
  /// @brief Opens and fully parses a SolidWorks file.
  ///
  /// Reads the file at @p path into memory, detects its format, and decodes
  /// all supported streams.  The following data is extracted when available:
  ///  - Global and per-configuration custom properties
  ///  - Configuration names and indices
  ///  - Precomputed mass properties
  ///  - Assembly component references and suppression state
  ///  - Weldment cut-list items
  ///  - Drawing sheet names and model-view references
  ///  - Preview PNG thumbnails (document-level and per-configuration)
  ///
  /// @param path Filesystem path to the SolidWorks file.  The document type
  ///             (part / assembly / drawing) is inferred from the extension.
  ///
  /// @return `Result::Ok(doc)` on success.
  /// @return `Result::Err(message)` on any of the following:
  ///   - The file cannot be opened or read.
  ///   - The file has an unsupported format (OLE2/pre-2015, ZIP/OPC).
  ///   - The file header is corrupt or unrecognised.
  ///
  /// @note Malformed *streams* inside an otherwise valid file do not cause
  ///       failure; they are silently skipped and the corresponding fields in
  ///       the returned @ref Document are left empty.
  [[nodiscard]] static Result<SwxDocument> Open(
      const std::filesystem::path& path);

  /// @brief Returns the fully parsed document.
  ///
  /// The returned reference is valid for the lifetime of this `SwxDocument`.
  [[nodiscard]] const Document& doc() const noexcept { return doc_; }

  /// @name Convenience accessors
  /// Thin wrappers around the corresponding fields of @ref doc().
  /// @{

  /// @brief Returns the document type (part, assembly, or drawing).
  [[nodiscard]] DocumentType type() const noexcept { return doc_.type; }

  /// @brief Returns the internal model version number (0 if absent).
  /// @see Document::version for the mapping between version numbers and
  ///      SolidWorks release years.
  [[nodiscard]] int version() const noexcept { return doc_.version; }

  /// @brief Returns the file-level custom properties.
  /// @see Document::global_properties
  [[nodiscard]] const std::map<std::string, std::string>& global_properties()
      const noexcept {
    return doc_.global_properties;
  }

  /// @brief Returns the list of configurations (empty for drawings).
  /// @see Document::configurations
  [[nodiscard]] const std::vector<Configuration>& configurations()
      const noexcept {
    return doc_.configurations;
  }

  /// @brief Returns the list of drawing sheets (empty for parts and assemblies).
  /// @see Document::sheets
  [[nodiscard]] const std::vector<Sheet>& sheets() const noexcept {
    return doc_.sheets;
  }

  /// @brief Returns the document-level preview PNG (may be empty).
  /// @see Document::preview_png
  [[nodiscard]] const std::vector<uint8_t>& preview_png() const noexcept {
    return doc_.preview_png;
  }

  /// @}

 private:
  explicit SwxDocument(Document doc) : doc_(std::move(doc)) {}

  Document doc_;
};

}  // namespace openswx
