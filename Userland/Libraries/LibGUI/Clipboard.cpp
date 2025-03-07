/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Clipboard/ClipboardClientEndpoint.h>
#include <Clipboard/ClipboardServerEndpoint.h>
#include <LibGUI/Clipboard.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/ServerConnection.h>

namespace GUI {

class ClipboardServerConnection final
    : public IPC::ServerConnection<ClipboardClientEndpoint, ClipboardServerEndpoint>
    , public ClipboardClientEndpoint {
    C_OBJECT(ClipboardServerConnection);

private:
    ClipboardServerConnection()
        : IPC::ServerConnection<ClipboardClientEndpoint, ClipboardServerEndpoint>(*this, "/tmp/portal/clipboard")
    {
    }

    virtual void clipboard_data_changed(String const& mime_type) override
    {
        Clipboard::the().clipboard_data_changed({}, mime_type);
    }
};

static ClipboardServerConnection* s_connection;

static ClipboardServerConnection& connection()
{
    return *s_connection;
}

void Clipboard::initialize(Badge<Application>)
{
    s_connection = &ClipboardServerConnection::construct().leak_ref();
}

Clipboard& Clipboard::the()
{
    static Clipboard* s_the;
    if (!s_the)
        s_the = new Clipboard;
    return *s_the;
}

Clipboard::DataAndType Clipboard::data_and_type() const
{
    auto response = connection().get_clipboard_data();
    if (!response.data().is_valid())
        return {};
    auto data = ByteBuffer::copy(response.data().data<void>(), response.data().size());
    if (!data.has_value())
        return {};

    auto type = response.mime_type();
    auto metadata = response.metadata().entries();
    return { data.release_value(), type, metadata };
}

RefPtr<Gfx::Bitmap> Clipboard::bitmap() const
{
    auto clipping = data_and_type();

    if (clipping.mime_type != "image/x-serenityos")
        return nullptr;

    auto width = clipping.metadata.get("width").value_or("0").to_uint();
    if (!width.has_value() || width.value() == 0)
        return nullptr;

    auto height = clipping.metadata.get("height").value_or("0").to_uint();
    if (!height.has_value() || height.value() == 0)
        return nullptr;

    auto scale = clipping.metadata.get("scale").value_or("0").to_uint();
    if (!scale.has_value() || scale.value() == 0)
        return nullptr;

    auto pitch = clipping.metadata.get("pitch").value_or("0").to_uint();
    if (!pitch.has_value() || pitch.value() == 0)
        return nullptr;

    auto format = clipping.metadata.get("format").value_or("0").to_uint();
    if (!format.has_value() || format.value() == 0)
        return nullptr;

    auto clipping_bitmap = Gfx::Bitmap::try_create_wrapper((Gfx::BitmapFormat)format.value(), { (int)width.value(), (int)height.value() }, scale.value(), pitch.value(), clipping.data.data());
    auto bitmap = Gfx::Bitmap::try_create(Gfx::BitmapFormat::BGRA8888, { (int)width.value(), (int)height.value() }, scale.value());

    for (int y = 0; y < clipping_bitmap->physical_height(); ++y) {
        for (int x = 0; x < clipping_bitmap->physical_width(); ++x) {
            auto pixel = clipping_bitmap->get_pixel(x, y);
            bitmap->set_pixel(x, y, pixel);
        }
    }

    return bitmap;
}

void Clipboard::set_data(ReadonlyBytes const& data, String const& type, HashMap<String, String> const& metadata)
{
    auto buffer = Core::AnonymousBuffer::create_with_size(data.size());
    if (!buffer.is_valid()) {
        dbgln("GUI::Clipboard::set_data() failed to create a buffer");
        return;
    }
    if (!data.is_empty())
        memcpy(buffer.data<void>(), data.data(), data.size());

    connection().async_set_clipboard_data(move(buffer), type, metadata);
}

void Clipboard::set_bitmap(Gfx::Bitmap const& bitmap)
{
    HashMap<String, String> metadata;
    metadata.set("width", String::number(bitmap.width()));
    metadata.set("height", String::number(bitmap.height()));
    metadata.set("scale", String::number(bitmap.scale()));
    metadata.set("format", String::number((int)bitmap.format()));
    metadata.set("pitch", String::number(bitmap.pitch()));
    set_data({ bitmap.scanline(0), bitmap.size_in_bytes() }, "image/x-serenityos", metadata);
}

void Clipboard::clear()
{
    connection().async_set_clipboard_data({}, {}, {});
}

void Clipboard::clipboard_data_changed(Badge<ClipboardServerConnection>, String const& mime_type)
{
    if (on_change)
        on_change(mime_type);
    for (auto* client : m_clients)
        client->clipboard_content_did_change(mime_type);
}

Clipboard::ClipboardClient::ClipboardClient()
{
    Clipboard::the().register_client({}, *this);
}

Clipboard::ClipboardClient::~ClipboardClient()
{
    Clipboard::the().unregister_client({}, *this);
}

}
