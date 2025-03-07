/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCompress/Gzip.h>
#include <LibCoredump/Reader.h>
#include <signal_numbers.h>
#include <string.h>

namespace Coredump {

OwnPtr<Reader> Reader::create(const String& path)
{
    auto file_or_error = MappedFile::map(path);
    if (file_or_error.is_error())
        return {};

    if (!Compress::GzipDecompressor::is_likely_compressed(file_or_error.value()->bytes())) {
        // It's an uncompressed coredump.
        return AK::adopt_own_if_nonnull(new (nothrow) Reader(file_or_error.release_value()));
    }

    auto decompressed_data = decompress_coredump(file_or_error.value()->bytes());
    if (!decompressed_data.has_value())
        return {};
    return adopt_own_if_nonnull(new (nothrow) Reader(decompressed_data.release_value()));
}

Reader::Reader(ByteBuffer buffer)
    : Reader(buffer.bytes())
{
    m_coredump_buffer = move(buffer);
}

Reader::Reader(NonnullRefPtr<MappedFile> file)
    : Reader(file->bytes())
{
    m_mapped_file = move(file);
}

Reader::Reader(ReadonlyBytes coredump_bytes)
    : m_coredump_bytes(coredump_bytes)
    , m_coredump_image(m_coredump_bytes)
{
    size_t index = 0;
    m_coredump_image.for_each_program_header([this, &index](auto pheader) {
        if (pheader.type() == PT_NOTE) {
            m_notes_segment_index = index;
            return IterationDecision::Break;
        }
        ++index;
        return IterationDecision::Continue;
    });
    VERIFY(m_notes_segment_index != -1);
}

Optional<ByteBuffer> Reader::decompress_coredump(const ReadonlyBytes& raw_coredump)
{
    auto decompressed_coredump = Compress::GzipDecompressor::decompress_all(raw_coredump);
    if (!decompressed_coredump.has_value())
        return ByteBuffer::copy(raw_coredump); // if we didn't manage to decompress it, try and parse it as decompressed coredump
    return decompressed_coredump;
}

Reader::~Reader()
{
}

Reader::NotesEntryIterator::NotesEntryIterator(const u8* notes_data)
    : m_current((const ELF::Core::NotesEntry*)notes_data)
    , start(notes_data)
{
}

ELF::Core::NotesEntryHeader::Type Reader::NotesEntryIterator::type() const
{
    VERIFY(m_current->header.type == ELF::Core::NotesEntryHeader::Type::ProcessInfo
        || m_current->header.type == ELF::Core::NotesEntryHeader::Type::MemoryRegionInfo
        || m_current->header.type == ELF::Core::NotesEntryHeader::Type::ThreadInfo
        || m_current->header.type == ELF::Core::NotesEntryHeader::Type::Metadata
        || m_current->header.type == ELF::Core::NotesEntryHeader::Type::Null);
    return m_current->header.type;
}

const ELF::Core::NotesEntry* Reader::NotesEntryIterator::current() const
{
    return m_current;
}

void Reader::NotesEntryIterator::next()
{
    VERIFY(!at_end());
    switch (type()) {
    case ELF::Core::NotesEntryHeader::Type::ProcessInfo: {
        const auto* current = reinterpret_cast<const ELF::Core::ProcessInfo*>(m_current);
        m_current = reinterpret_cast<const ELF::Core::NotesEntry*>(current->json_data + strlen(current->json_data) + 1);
        break;
    }
    case ELF::Core::NotesEntryHeader::Type::ThreadInfo: {
        const auto* current = reinterpret_cast<const ELF::Core::ThreadInfo*>(m_current);
        m_current = reinterpret_cast<const ELF::Core::NotesEntry*>(current + 1);
        break;
    }
    case ELF::Core::NotesEntryHeader::Type::MemoryRegionInfo: {
        const auto* current = reinterpret_cast<const ELF::Core::MemoryRegionInfo*>(m_current);
        m_current = reinterpret_cast<const ELF::Core::NotesEntry*>(current->region_name + strlen(current->region_name) + 1);
        break;
    }
    case ELF::Core::NotesEntryHeader::Type::Metadata: {
        const auto* current = reinterpret_cast<const ELF::Core::Metadata*>(m_current);
        m_current = reinterpret_cast<const ELF::Core::NotesEntry*>(current->json_data + strlen(current->json_data) + 1);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

bool Reader::NotesEntryIterator::at_end() const
{
    return type() == ELF::Core::NotesEntryHeader::Type::Null;
}

Optional<FlatPtr> Reader::peek_memory(FlatPtr address) const
{
    const auto* region = region_containing(address);
    if (!region)
        return {};

    FlatPtr offset_in_region = address - region->region_start;
    const char* region_data = image().program_header(region->program_header_index).raw_data();
    return *(const FlatPtr*)(&region_data[offset_in_region]);
}

const JsonObject Reader::process_info() const
{
    const ELF::Core::ProcessInfo* process_info_notes_entry = nullptr;
    for (NotesEntryIterator it((const u8*)m_coredump_image.program_header(m_notes_segment_index).raw_data()); !it.at_end(); it.next()) {
        if (it.type() != ELF::Core::NotesEntryHeader::Type::ProcessInfo)
            continue;
        process_info_notes_entry = reinterpret_cast<const ELF::Core::ProcessInfo*>(it.current());
        break;
    }
    if (!process_info_notes_entry)
        return {};
    auto process_info_json_value = JsonValue::from_string(process_info_notes_entry->json_data);
    if (!process_info_json_value.has_value())
        return {};
    if (!process_info_json_value.value().is_object())
        return {};
    return process_info_json_value.value().as_object();
    // FIXME: Maybe just cache this on the Reader instance after first access.
}

ELF::Core::MemoryRegionInfo const* Reader::first_region_for_object(StringView object_name) const
{
    ELF::Core::MemoryRegionInfo const* ret = nullptr;
    for_each_memory_region_info([&ret, &object_name](auto& region_info) {
        if (region_info.object_name() == object_name) {
            ret = &region_info;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return ret;
}

const ELF::Core::MemoryRegionInfo* Reader::region_containing(FlatPtr address) const
{
    const ELF::Core::MemoryRegionInfo* ret = nullptr;
    for_each_memory_region_info([&ret, address](const ELF::Core::MemoryRegionInfo& region_info) {
        if (region_info.region_start <= address && region_info.region_end >= address) {
            ret = &region_info;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return ret;
}

int Reader::process_pid() const
{
    auto process_info = this->process_info();
    auto pid = process_info.get("pid");
    return pid.to_number<int>();
}

u8 Reader::process_termination_signal() const
{
    auto process_info = this->process_info();
    auto termination_signal = process_info.get("termination_signal");
    auto signal_number = termination_signal.to_number<int>();
    if (signal_number <= SIGINVAL || signal_number >= NSIG)
        return SIGINVAL;
    return (u8)signal_number;
}

String Reader::process_executable_path() const
{
    auto process_info = this->process_info();
    auto executable_path = process_info.get("executable_path");
    return executable_path.as_string_or({});
}

Vector<String> Reader::process_arguments() const
{
    auto process_info = this->process_info();
    auto arguments = process_info.get("arguments");
    if (!arguments.is_array())
        return {};
    Vector<String> vector;
    arguments.as_array().for_each([&](auto& value) {
        if (value.is_string())
            vector.append(value.as_string());
    });
    return vector;
}

Vector<String> Reader::process_environment() const
{
    auto process_info = this->process_info();
    auto environment = process_info.get("environment");
    if (!environment.is_array())
        return {};
    Vector<String> vector;
    environment.as_array().for_each([&](auto& value) {
        if (value.is_string())
            vector.append(value.as_string());
    });
    return vector;
}

HashMap<String, String> Reader::metadata() const
{
    const ELF::Core::Metadata* metadata_notes_entry = nullptr;
    for (NotesEntryIterator it((const u8*)m_coredump_image.program_header(m_notes_segment_index).raw_data()); !it.at_end(); it.next()) {
        if (it.type() != ELF::Core::NotesEntryHeader::Type::Metadata)
            continue;
        metadata_notes_entry = reinterpret_cast<const ELF::Core::Metadata*>(it.current());
        break;
    }
    if (!metadata_notes_entry)
        return {};
    auto metadata_json_value = JsonValue::from_string(metadata_notes_entry->json_data);
    if (!metadata_json_value.has_value())
        return {};
    if (!metadata_json_value.value().is_object())
        return {};
    HashMap<String, String> metadata;
    metadata_json_value.value().as_object().for_each_member([&](auto& key, auto& value) {
        metadata.set(key, value.as_string_or({}));
    });
    return metadata;
}

struct LibraryData {
    String name;
    OwnPtr<MappedFile> file;
    ELF::Image lib_elf;
};

const Reader::LibraryData* Reader::library_containing(FlatPtr address) const
{
    static HashMap<String, OwnPtr<LibraryData>> cached_libs;
    auto* region = region_containing(address);
    if (!region)
        return {};

    auto name = region->object_name();

    String path;
    if (name.contains(".so"))
        path = String::formatted("/usr/lib/{}", name);
    else {
        path = name;
    }

    if (!cached_libs.contains(path)) {
        auto file_or_error = MappedFile::map(path);
        if (file_or_error.is_error())
            return {};
        auto image = ELF::Image(file_or_error.value()->bytes());
        cached_libs.set(path, make<LibraryData>(name, (FlatPtr)region->region_start, file_or_error.release_value(), move(image)));
    }

    auto lib_data = cached_libs.get(path).value();
    return lib_data;
}

}
