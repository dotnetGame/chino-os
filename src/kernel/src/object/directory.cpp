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
#include <chino/directory.h>
#include <cstring>

using namespace chino;
using namespace chino::ob;

const object_type wellknown_types::directory({ .operations = {} });

namespace
{
static_object<directory> root_directory_(wellknown_types::directory);
}

result<void, error_code> directory::insert_entry_nolock(object_header &header) noexcept
{
    auto node = entries_.first_nolock();
    while (node)
    {
        auto cmp = std::strncmp(node->owner()->name, header.name, MAX_OBJECT_NAME);
        if (cmp == 0)
            return err(error_code::key_already_exists);
        else if (cmp > 0)
            break;
        node = node->next;
    }

    entries_.insert_before_nolock(node, &header.directory_entry);
    return ok();
}

directory &ob::root_directory() noexcept
{
    return root_directory_.get();
}
