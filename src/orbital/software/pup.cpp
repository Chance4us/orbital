/**
 * PUP format.
 *
 * Copyright 2017-2021. Orbital project.
 * Released under MIT license. Read LICENSE for more details.
 *
 * Authors:
 * - Alexandro Sanchez Bach <alexandro@phi.nz>
 */

#include "pup.h"
#include <orbital/crypto_ps4.h>

#include <botan/botan.h>
#include <zlib.h>

#include <stdexcept>

constexpr U32 PUP_MAGIC = 0x1D3D154F;

enum PupEndian {
    LITTLE = 1,
};

enum PupFlags {
    JIG = 1,
};

// PUP decryption
static void pup_decrypt(Buffer& buffer, const PupSegmentMeta& meta) {
    Botan::SymmetricKey key(meta.data_key, 16);
    Botan::InitializationVector iv(meta.data_iv, 16);
    auto cipher = Botan::get_cipher("AES-128/CBC/NoPadding", key, iv, Botan::Cipher_Dir::DECRYPTION);

    const auto size_aligned = buffer.size() & ~0xF;
    const auto overflow = buffer.size() & 0xF;
    U08 prev_block[16];
    U08 next_block[16];
    if (overflow && size_aligned >= 16) {
        memcpy(prev_block, &buffer[size_aligned - 16], 16);
    }

    Botan::Pipe pipe(cipher);
    pipe.start_msg();
    pipe.write(buffer.data(), size_aligned);
    pipe.end_msg();
    pipe.read(buffer.data(), size_aligned);

    // Apply custom CTS if unaligned
    if (overflow) {
        auto cipher_enc = Botan::get_cipher("AES-128/CBC", key, Botan::Cipher_Dir::ENCRYPTION);
        Botan::Pipe pipe_enc(cipher_enc);
        pipe_enc.start_msg();
        pipe_enc.write(prev_block, 16);
        pipe_enc.end_msg();
        pipe_enc.read(next_block, 16);
        for (size_t i = 0; i < overflow; i++) {
            buffer[size_aligned + i] ^= next_block[i];
        }
    }
}

// PUP parser
PupParser::PupParser(Stream& s, bool verify) : s(s) {
    Buffer buffer;
    const auto& crypto = ps4Crypto();

    // Read and verify PUP header
    s.seek(0, StreamSeek::Set);
    header = s.read_t<PupHeader>();
    assert(header.magic == PUP_MAGIC);
    assert(header.version == 0);
    assert(header.mode == 1);
    assert(header.endian == PupEndian::LITTLE);
    assert(header.attr == 0x12);

    // Discard unsupported flags
    assert((header.flags & PupFlags::JIG) == 0, "Unsupported JIG flag");

    // Decrypt and cache header
    buffer.resize(header.hdr_size - sizeof(PupHeader));
    s.read(buffer.size(), buffer.data());
    crypto.decrypt(buffer.data(), buffer.size(), crypto.get("pup.hdr"));
    headerEx = reinterpret_cast<const PupHeaderEx&>(buffer[0]);
    auto* segment_entries = reinterpret_cast<const PupSegmentEntry*>(&buffer[sizeof(PupHeaderEx)]);
    segEntries.clear();
    for (size_t i = 0; i < headerEx.segment_count; i++) {
        segEntries.push_back(segment_entries[i]);
    }

    // Decrypt and cache meta
    buffer.resize(header.meta_size);
    s.read(buffer.size(), buffer.data());
    crypto.decrypt(buffer.data(), buffer.size(), crypto.get("pup.root_key"));
    auto* meta_entries = reinterpret_cast<const PupSegmentMeta*>(&buffer[0]);
    segMetas.clear();
    for (size_t i = 0; i < headerEx.segment_count; i++) {
        segMetas.push_back(meta_entries[i]);
    }

    if (verify) {
        throw std::runtime_error("Unimplemented");
    }
}

PupParser::~PupParser() {
}

Buffer PupParser::get(U64 id) {
    const auto index = find(id);
    if (segEntries[index].has_blocks()) {
        return get_blocked(index);
    } else {
        return get_nonblocked(index);
    }
}

Buffer PupParser::get_blocked(U64 index) {
    const auto& crypto = ps4Crypto();

    // Get target segment
    const PupSegmentEntry& entry = segEntries[index];
    const PupSegmentMeta& meta = segMetas[index];
    const auto block_size = entry.block_size();
    const auto block_count = entry.block_count();

    // Get information segment
    const auto info_index = find_info(index);
    const PupSegmentEntry& info_entry = segEntries[info_index];
    const PupSegmentMeta& info_meta = segMetas[info_index];

    // Read and process information segment data
    Buffer info_buffer(info_entry.file_size);
    s.seek(info_entry.offset, StreamSeek::Set);
    s.read(info_buffer.size(), info_buffer.data());
    if (info_entry.is_encrypted()) {
        pup_decrypt(info_buffer, info_meta);
    }
    if (info_entry.is_compressed()) {
        throw std::runtime_error("Unimplemented");
    }
    if (info_entry.is_signed()) {
        // TODO: throw std::runtime_error("Unimplemented");
    }

    BufferStream info_stream(std::move(info_buffer));
    std::vector<PupDigest> digests;
    std::vector<PupExtent> extents;
    if (info_entry.has_digests()) {
        for (size_t i = 0; i < block_count; i++) {
            digests.push_back(info_stream.read_t<PupDigest>());
        }
    }
    if (info_entry.has_extents()) {
        for (size_t i = 0; i < block_count; i++) {
            extents.push_back(info_stream.read_t<PupExtent>());
        }
    }

    // Process target segment
    auto left_size = entry.file_size;
    Buffer block, segment;
    for (const auto& extent : extents) {
        block.resize(extent.size);
        s.seek(entry.offset + extent.offset, StreamSeek::Set);
        s.read(extent.size, block.data());

        const auto cur_zsize = (extent.size & ~0xF) - (extent.size & 0xF);
        const auto cur_size = std::min(block_size, left_size);
        left_size -= cur_size;
        if (entry.is_signed()) {
            // TODO: throw std::runtime_error("Unimplemented");
        }
        if (entry.is_encrypted()) {
            Key key(Key::AES_128_CBC, meta.data_key, 16, meta.data_iv, 16);
            crypto.decrypt(block.data(), block.size(), key);
        }
        segment.resize(segment.size() + cur_size);
        U08* dest = segment.data() + segment.size() - cur_size;
        if (entry.is_compressed()) {
            unsigned long cur_usize = cur_size;
            uncompress(dest, &cur_usize, block.data(), cur_zsize);
        } else {
            memcpy(dest, block.data(), block.size());
        }
    }
    return segment;
}

Buffer PupParser::get_nonblocked(U64 index) {
    throw std::runtime_error("Unimplemented");
}

U64 PupParser::find(const std::function<bool(const PupSegmentEntry&, const PupSegmentMeta&)>& pred) const {
    for (size_t i = 0; i < headerEx.segment_count; i++) {
        if (pred(segEntries[i], segMetas[i])) {
            return i;
        }
    }
    throw std::out_of_range("PUP segment not found");
}

U64 PupParser::find(U64 id) const {
    return find([=](const PupSegmentEntry& entry, const PupSegmentMeta& meta) -> bool {
        return entry.id() == id && !entry.is_info();
    });
}

U64 PupParser::find_info(U64 id) const {
    return find([=](const PupSegmentEntry& entry, const PupSegmentMeta& meta) -> bool {
        return entry.id() == id && entry.is_info();
    });
}
