// SPDX-License-Identifier: GPL-3.0-or-later
//
// services/ProjectStore.cpp - the public project-store surface declared in
// ProjectStore.hpp (Requirements 3.5, 3.6): serialize and deserialize a Project to
// and from the documented `.palmier` JSON document, and the file-level save and
// load that wrap them with I/O errors.
//
// The JSON reader and writer, the per-entity readers and writers and the format
// vocabulary were moved into sibling units when this file exceeded 1000 lines;
// they share services/ProjectStoreInternal.hpp. This file is what callers see.

#include "services/ProjectStoreInternal.hpp"

namespace palmier::services {

using namespace palmier::services::detail;  // NOLINT(google-build-using-namespace)

std::string serializeProject(const Project& project) {
    JsonWriter w;
    w.beginObject();
    w.key("format");
    w.valueString(kFormatMagic);
    w.key("version");
    w.valueString(project.version.toString());
    w.key("project");
    writeProject(w, project);
    w.endObject();
    return w.take();
}

Result<Project> deserializeProject(std::string_view text) {
    try {
        JsonParser parser{text};
        const JsonValue root = parser.parse();
        if (!root.isObject()) {
            return invalidArgument("`.palmier` document root must be a JSON object");
        }

        // Verify the format magic so we don't misinterpret arbitrary JSON.
        const std::string format = requireString(root, "format");
        if (format != kFormatMagic) {
            return invalidArgument("not a `.palmier` document (unexpected format '" + format +
                                   "')");
        }

        // Read and enforce schema-version compatibility before building the project.
        const std::string versionText = requireString(root, "version");
        const std::optional<SchemaVersion> version = SchemaVersion::parse(versionText);
        if (!version.has_value()) {
            return invalidArgument("malformed schema version '" + versionText + "'");
        }
        if (!version->isSupported()) {
            return unsupported("`.palmier` schema version " + version->toString() +
                               " is not supported by this build (current " +
                               SchemaVersion::current().toString() + ")");
        }

        Project project = readProject(requireMember(root, "project"));
        project.version = *version;
        return project;
    } catch (const JsonParseError& e) {
        return invalidArgument("malformed `.palmier` document: " + e.message);
    }
}

Result<void> saveProjectToFile(const Project& project, const std::filesystem::path& path) {
    const std::string text = serializeProject(project);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return makeError(ErrorCode::Io, "could not open '" + path.string() + "' for writing");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        return makeError(ErrorCode::Io, "failed while writing '" + path.string() + "'");
    }
    return ok();
}

Result<Project> loadProjectFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::Io, "could not open '" + path.string() + "' for reading");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return makeError(ErrorCode::Io, "failed while reading '" + path.string() + "'");
    }
    return deserializeProject(buffer.str());
}

}  // namespace palmier::services
