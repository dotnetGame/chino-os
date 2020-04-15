// MIT License
//
// Copyright (c) 2020 SunnyCase
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include <chino/ddk/directory.h>
#include <chino/ddk/kernel.h>
#include <chino/io.h>
#include <chino/io/io_manager.h>
#include <chino/threading/scheduler.h>
#include <libfdt.h>
#include <ulog.h>

using namespace chino;
using namespace chino::ob;
using namespace chino::io;
using namespace std::string_view_literals;

#if defined(_MSC_VER)
#pragma comment(linker, "/merge:.CHDRV=.rdata")

__declspec(allocate(".CHDRV$A")) const ::chino::io::driver *drivers_begin_[] = { nullptr };
__declspec(allocate(".CHDRV$Z")) const ::chino::io::driver *drivers_end_[] = { nullptr };
#elif defined(__GNUC__)
extern "C"
{
    extern const ::chino::io::driver *drivers_begin_[];
    extern const ::chino::io::driver *drivers_end_[];
}
#else
#error "Unsupported compiler"
#endif

struct default_device
{
};

machine_desc io::machine_desc_;

#define DEFINE_DEV_TYPE(x, v) CHINO_CONCAT(#x, sv),
static std::string_view dev_type_prefixes_[] = {
#include <chino/ddk/device_types.def>
};
#undef DEFINE_DEV_TYPE

static std::atomic<uint16_t> device_counts_[(size_t)device_type::COUNT];
static handle_t dev_root_;
static default_device default_devs_;
static driver_id attach_drv_id;

static void setup_machine_desc(const void *fdt, int node) noexcept
{
    device_descriptor root(node);
    machine_desc_.fdt = fdt;
    machine_desc_.model = root.property("model").unwrap().string();

    int child;
    fdt_for_each_subnode(child, machine_desc_.fdt, node)
    {
        device_descriptor cnode(child);
        auto bootargs = cnode.property("bootargs");
        if (bootargs.is_ok())
        {
            machine_desc_.bootargs = bootargs.unwrap().string();
            break;
        }
    }
}

static result<void, error_code> populate_devices(int node) noexcept
{
    int child;
    fdt_for_each_subnode(child, machine_desc_.fdt, node)
        try_(probe_device({ child }, nullptr));
    return ok();
}

static result<device_id, error_code> create_device_id(const device_descriptor &node, device *parent) noexcept
{
    auto compats = node.property("compatible").unwrap();
    size_t compat_id = 0;
    const device_id *dev_id = nullptr;

    while (true)
    {
        auto compat = compats.string(compat_id++);
        if (compat.empty())
            break;
        auto drv = drivers_begin_;
        while (drv < drivers_end_)
        {
            if (*drv)
            {
                auto id = (*drv)->check_compatible(compat);
                if (id)
                    return ok<device_id>(node.node(), parent, **drv, *id);
            }

            drv++;
        }
    }

    ULOG_CRITICAL("Cannot find driver for %s\n", compats.string(0).data());
    return err(error_code::not_found);
}

static result<std::string_view, error_code> make_dev_prefix(device_type type) noexcept
{
    if (size_t(type) < size_t(device_type::COUNT))
        return ok(dev_type_prefixes_[size_t(type)]);
    return err(error_code::invalid_argument);
}

static result<std::string_view, error_code> make_dev_name(device_type type, char *buffer, size_t buffer_len) noexcept
{
    if (size_t(type) < size_t(device_type::COUNT))
    {
        auto id = device_counts_[size_t(type)]++;
        auto n = std::snprintf(buffer, buffer_len, "%s%d", make_dev_prefix(type).unwrap().data(), id);
        if (n > 0 && n < buffer_len)
            return ok<std::string_view>(buffer, n);
        return err(error_code::insufficient_buffer);
    }

    return err(error_code::invalid_argument);
}

static result<void, error_code> attach_device(device &dev, std::string_view args) noexcept
{
    auto type = dev.type;
    auto drv = drivers_begin_;
    while (++drv < drivers_end_)
    {
        if (*drv && (*drv)->ops.attach_device)
        {
            auto ret = (*drv)->ops.attach_device(**drv, dev, args);
            if (ret.is_ok())
                return ok();
        }
    }

    ULOG_CRITICAL("Cannot attch driver for /dev/%s\n", dev.header().name);
    return err(error_code::not_supported);
}

static result<void, error_code> setup_console() noexcept
{
    if (!machine_desc_.bootargs.empty())
    {
        // find console
        // split by space
        auto remaining = machine_desc_.bootargs;
        while (!remaining.empty())
        {
            auto entry_end = remaining.find_first_of(' ');
            auto entry = entry_end == std::string_view::npos ? remaining : remaining.substr(0, entry_end);
            remaining = entry_end == std::string_view::npos ? std::string_view {} : remaining.substr(entry_end + 1);

            // split by =
            auto key_end = entry.find_first_of('=');
            if (key_end == std::string_view::npos)
                panic("invalid bootargs");
            auto key = entry.substr(0, key_end);
            if (key == "console")
            {
                auto value = entry.substr(key_end + 1);
                auto dev_name_end = value.find_first_of(',');
                auto dev_name = dev_name_end == std::string_view::npos ? value : value.substr(0, dev_name_end);
                auto args = dev_name_end == std::string_view::npos ? std::string_view {} : value.substr(dev_name_end + 1);
                try_var(dev, ob::reference_object<device>({ .root = dev_root_, .name = dev_name, .desired_access = access_mask::generic_all }, wellknown_types::device));
                return attach_device(*dev, args);
            }
        }
    }

    return err(error_code::argument_null);
}

machine_desc io::get_machine_desc() noexcept
{
    return machine_desc_;
}

result<void, error_code> io::probe_device(const device_descriptor &node, device *parent) noexcept
{
    if (node.has_compatible())
    {
        auto dev_id_r = create_device_id(node, parent);
        if (dev_id_r.is_err())
            return err(dev_id_r.unwrap_err());
        auto dev_id = dev_id_r.unwrap();
        try_(dev_id.drv().ops.add_device(dev_id.drv(), dev_id));
    }
    else
    {
        int child;
        fdt_for_each_subnode(child, machine_desc_.fdt, node.node())
            try_(probe_device({ child }, parent));
    }

    return ok();
}

result<void, error_code> io::populate_sub_devices(device &parent) noexcept
{
    auto parent_id = parent.id.node();

    int child;
    fdt_for_each_subnode(child, machine_desc_.fdt, parent_id)
        try_(probe_device({ child }, &parent));
    return ok();
}

result<device *, error_code> io::create_device(const device_id &id, device_type type, size_t extension_size) noexcept
{
    try_var(ob, create_object(wellknown_types::device, sizeof(device) + extension_size));
    auto dev = static_cast<device *>(ob);
    new (dev) device { {}, id, type };

    char namebuf[MAX_OBJECT_NAME + 1];
    try_var(name, make_dev_name(type, namebuf, std::size(namebuf)));
    try_(insert_object(*dev, { .root = dev_root_, .name = name, .desired_access = access_mask::generic_all }));
    return ok(static_cast<device *>(dev));
}

result<device *, error_code> io::create_device(std::string_view name, const driver &drv, device_type type, size_t extension_size) noexcept
{
    try_var(ob, create_object(wellknown_types::device, sizeof(device) + extension_size));
    auto dev = static_cast<device *>(ob);
    device_id devid(-1, nullptr, drv, attach_drv_id);
    new (dev) device { {}, devid, type };

    char namebuf[MAX_OBJECT_NAME + 1];
    if (name.empty())
        try_var(name, make_dev_name(type, namebuf, std::size(namebuf)));
    try_(insert_object(*dev, { .root = dev_root_, .name = name, .desired_access = access_mask::generic_all }));
    return ok(static_cast<device *>(dev));
}

result<device *, error_code> io::create_device(const driver &drv, device_type type, size_t extension_size) noexcept
{
    return create_device({}, type, extension_size);
}

result<file *, error_code> io::create_file(device &dev, size_t extension_size) noexcept
{
    try_var(ob, create_object(wellknown_types::file, sizeof(file) + extension_size));
    auto f = static_cast<file *>(ob);
    new (f) file { {}, dev, 0 };
    return ok(f);
}

result<file *, error_code> io::open_file(device &dev, access_mask access, std::string_view filename, create_disposition create_disp) noexcept
{
    auto &drv = dev.id.drv();
    if (!drv.ops.open_device)
        return err(error_code::not_supported);

    std::unique_lock lock(dev.syncroot);
    return drv.ops.open_device(drv, dev, filename, create_disp);
}

result<size_t, error_code> io::read_file(file &file, gsl::span<gsl::byte> buffer) noexcept
{
    auto &drv = file.dev.id.drv();
    if (!drv.ops.read_device)
        return err(error_code::not_supported);

    std::unique_lock lock(file.syncroot);
    try_var(ret, drv.ops.read_device(drv, file.dev, file, buffer));
    file.offset += ret;
    return ok(ret);
}

result<void, error_code> io::write_file(file &file, gsl::span<const gsl::byte> buffer) noexcept
{
    auto &drv = file.dev.id.drv();
    if (!drv.ops.write_device)
        return err(error_code::not_supported);

    std::unique_lock lock(file.syncroot);
    try_(drv.ops.write_device(drv, file.dev, file, buffer));
    file.offset += buffer.length_bytes();
    return ok();
}

result<void, error_code> io::close_file(file &file) noexcept
{
    auto &drv = file.dev.id.drv();
    if (!drv.ops.close_device)
        return ok();

    std::unique_lock lock(file.syncroot);
    return drv.ops.close_device(drv, file.dev, file);
}

result<void, error_code> io::alloc_console() noexcept
{
    auto &ps = threading::current_process();

    std::unique_lock lock(ps.syncroot);
    if (ps.stdin_ == handle_t::invalid())
    {
        try_var(dev, ob::reference_object<device>({ .name = "/dev/console", .desired_access = access_mask::generic_all }, wellknown_types::device));
        try_var(file, open_file(*dev, access_mask::generic_all, {}));
        try_var(handle, insert_object(*file, { .desired_access = access_mask::generic_all }));
        ps.stdin_ = ps.stdout_ = ps.stderr_ = handle;
    }

    return ok();
}

handle_t io::get_std_handle(std_handles type) noexcept
{
    auto &ps = threading::current_process();

    switch (type)
    {
    case chino::io::std_handles::in:
        return ps.stdin_;
    case chino::io::std_handles::out:
        return ps.stdout_;
    case chino::io::std_handles::err:
        return ps.stderr_;
    default:
        return handle_t::invalid();
    }
}

result<handle_t, error_code> io::open(const insert_lookup_object_options &options, create_disposition create_disp) noexcept
{
    std::string_view remaining_name;
    try_var(dev, reference_object_partial<device>(options, remaining_name, wellknown_types::device));
    try_var(file, open_file(*dev, options.desired_access, remaining_name, create_disp));
    try_var(handle, insert_object(*file, { .desired_access = options.desired_access }));
    return ok(handle);
}

result<size_t, error_code> io::read(handle_t file, gsl::span<gsl::byte> buffer) noexcept
{
    try_var(f, ob::reference_object<io::file>(file, wellknown_types::file));
    return read_file(*f, buffer);
}

result<void, error_code> io::write(handle_t file, gsl::span<const gsl::byte> buffer) noexcept
{
    try_var(f, ob::reference_object<io::file>(file, wellknown_types::file));
    return write_file(*f, buffer);
}

result<void, error_code> io::close(handle_t file) noexcept
{
    try_var(f, ob::reference_object<io::file>(file, wellknown_types::file));
    try_(close_file(*f));
    try_(close_handle(file));
    return ok();
}

result<void, error_code> kernel::io_manager_init(gsl::span<const uint8_t> fdt)
{
    try_set(dev_root_, create_directory({ .name = "/dev", .desired_access = access_mask::generic_all }));

    if (fdt_check_full(fdt.data(), fdt.length_bytes()) != 0)
        return err(error_code::invalid_argument);

    auto root_node = fdt_next_node(fdt.data(), -1, nullptr);
    setup_machine_desc(fdt.data(), root_node);
    try_(populate_devices(root_node));

    try_(setup_console());
    return ok();
}