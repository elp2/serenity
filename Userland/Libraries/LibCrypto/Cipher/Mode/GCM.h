/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <LibCrypto/Authentication/GHash.h>
#include <LibCrypto/Cipher/Mode/CTR.h>
#include <LibCrypto/Verification.h>

namespace Crypto {
namespace Cipher {

using IncrementFunction = IncrementInplace;

template<typename T>
class GCM : public CTR<T, IncrementFunction> {
public:
    constexpr static size_t IVSizeInBits = 128;

    virtual ~GCM() = default;

    template<typename... Args>
    explicit constexpr GCM<T>(Args... args)
        : CTR<T>(args...)
    {
        static_assert(T::BlockSizeInBits == 128u, "GCM Mode is only available for 128-bit Ciphers");

        __builtin_memset(m_auth_key_storage, 0, block_size);
        typename T::BlockType key_block(m_auth_key_storage, block_size);
        this->cipher().encrypt_block(key_block, key_block);
        key_block.bytes().copy_to(m_auth_key);

        m_ghash = Authentication::GHash(m_auth_key);
    }

    virtual String class_name() const override
    {
        StringBuilder builder;
        builder.append(this->cipher().class_name());
        builder.append("_GCM");
        return builder.build();
    }

    virtual size_t IV_length() const override { return IVSizeInBits / 8; }

    // FIXME: This overload throws away the auth stuff, think up a better way to return more than a single bytebuffer.
    virtual void encrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}, Bytes* = nullptr) override
    {
        VERIFY(!ivec.is_empty());

        static ByteBuffer dummy;

        encrypt(in, out, ivec, dummy, dummy);
    }
    virtual void decrypt(ReadonlyBytes in, Bytes& out, ReadonlyBytes ivec = {}) override
    {
        encrypt(in, out, ivec);
    }

    void encrypt(const ReadonlyBytes& in, Bytes out, const ReadonlyBytes& iv_in, const ReadonlyBytes& aad, Bytes tag)
    {
        auto iv_buf_result = ByteBuffer::copy(iv_in);
        // Not enough memory to figure out :shrug:
        if (!iv_buf_result.has_value()) {
            dbgln("GCM::encrypt: Not enough memory to allocate {} bytes for IV", iv_in.size());
            return;
        }

        auto iv = iv_buf_result->bytes();

        // Increment the IV for block 0
        CTR<T>::increment(iv);
        typename T::BlockType block0;
        block0.overwrite(iv);
        this->cipher().encrypt_block(block0, block0);

        // Skip past block 0
        CTR<T>::increment(iv);

        if (in.is_empty())
            CTR<T>::key_stream(out, iv);
        else
            CTR<T>::encrypt(in, out, iv);

        auto auth_tag = m_ghash->process(aad, out);
        block0.apply_initialization_vector({ auth_tag.data, array_size(auth_tag.data) });
        block0.bytes().copy_to(tag);
    }

    VerificationConsistency decrypt(ReadonlyBytes in, Bytes out, ReadonlyBytes iv_in, ReadonlyBytes aad, ReadonlyBytes tag)
    {
        auto iv_buf_result = ByteBuffer::copy(iv_in);
        // Not enough memory to figure out :shrug:
        if (!iv_buf_result.has_value())
            return VerificationConsistency::Inconsistent;

        auto iv = iv_buf_result->bytes();

        // Increment the IV for block 0
        CTR<T>::increment(iv);
        typename T::BlockType block0;
        block0.overwrite(iv);
        this->cipher().encrypt_block(block0, block0);

        // Skip past block 0
        CTR<T>::increment(iv);

        auto auth_tag = m_ghash->process(aad, in);
        block0.apply_initialization_vector({ auth_tag.data, array_size(auth_tag.data) });

        auto test_consistency = [&] {
            if (block0.block_size() != tag.size() || __builtin_memcmp(block0.bytes().data(), tag.data(), tag.size()) != 0)
                return VerificationConsistency::Inconsistent;

            return VerificationConsistency::Consistent;
        };
        // FIXME: This block needs constant-time comparisons.

        if (in.is_empty()) {
            out = {};
            return test_consistency();
        }

        CTR<T>::encrypt(in, out, iv);
        return test_consistency();
    }

private:
    static constexpr auto block_size = T::BlockType::BlockSizeInBits / 8;
    u8 m_auth_key_storage[block_size];
    Bytes m_auth_key { m_auth_key_storage, block_size };
    Optional<Authentication::GHash> m_ghash;
};

}

}
