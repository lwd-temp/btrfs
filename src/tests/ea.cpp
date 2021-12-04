#include "test.h"

using namespace std;

static constexpr uint32_t ea_size(string_view name, string_view value) {
    uint32_t size = offsetof(FILE_FULL_EA_INFORMATION, EaName) + name.length() + 1 + value.length();

    if (size & 3)
        size = ((size >> 2) + 1) << 2;

    return size;
}

void write_ea(HANDLE h, string_view name, string_view value) {
    NTSTATUS Status;
    IO_STATUS_BLOCK iosb;
    vector<uint8_t> buf;

    buf.resize(ea_size(name, value));

    auto& ffeai = *(FILE_FULL_EA_INFORMATION*)buf.data();

    ffeai.NextEntryOffset = 0;
    ffeai.Flags = 0;
    ffeai.EaNameLength = name.size();
    ffeai.EaValueLength = value.size();

    memcpy(ffeai.EaName, name.data(), name.size());
    ffeai.EaName[name.size()] = 0;
    memcpy(ffeai.EaName + name.size() + 1, value.data(), value.size());

    Status = NtSetEaFile(h, &iosb, buf.data(), buf.size());

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);
}

template<size_t N>
static void write_eas(HANDLE h, const array<pair<string_view, string_view>, N>& arr) {
    NTSTATUS Status;
    IO_STATUS_BLOCK iosb;
    vector<uint8_t> buf;
    size_t size = 0;

    for (unsigned int i = 0; i < N; i++) {
        size += ea_size(arr[i].first, arr[i].second);
    }

    buf.resize(size);

    auto ptr = buf.data();

    for (unsigned int i = 0; i < N; i++) {
        auto& ffeai = *(FILE_FULL_EA_INFORMATION*)ptr;

        ffeai.NextEntryOffset = 0;
        ffeai.Flags = 0;
        ffeai.EaNameLength = arr[i].first.size();
        ffeai.EaValueLength = arr[i].second.size();

        memcpy(ffeai.EaName, arr[i].first.data(), arr[i].first.size());
        ffeai.EaName[arr[i].first.size()] = 0;
        memcpy(ffeai.EaName + arr[i].first.size() + 1, arr[i].second.data(), arr[i].second.size());

        if (i != N - 1) {
            ffeai.NextEntryOffset = ea_size(arr[i].first, arr[i].second);
            ptr += ffeai.NextEntryOffset;
        }
    }

    Status = NtSetEaFile(h, &iosb, buf.data(), buf.size());

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);
}

static vector<varbuf<FILE_FULL_EA_INFORMATION>> read_ea(HANDLE h) {
    NTSTATUS Status;
    IO_STATUS_BLOCK iosb;
    char buf[4096];

    Status = NtQueryEaFile(h, &iosb, buf, sizeof(buf), false, nullptr,
                           0, nullptr, true);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    vector<varbuf<FILE_FULL_EA_INFORMATION>> ret;
    auto ptr = buf;

    do {
        auto& ffeai = *(FILE_FULL_EA_INFORMATION*)ptr;

        ret.emplace_back();
        auto& item = ret.back();

        item.buf.resize(offsetof(FILE_FULL_EA_INFORMATION, EaName) + ffeai.EaNameLength + 1 + ffeai.EaValueLength);

        memcpy(item.buf.data(), &ffeai, item.buf.size());

        if (ffeai.NextEntryOffset == 0)
            break;

        ptr += ffeai.NextEntryOffset;
    } while (true);

    return ret;
}

static varbuf<FILE_ALL_INFORMATION> query_all_information(HANDLE h) {
    IO_STATUS_BLOCK iosb;
    NTSTATUS Status;
    FILE_ALL_INFORMATION fai;

    fai.NameInformation.FileNameLength = 0;

    Status = NtQueryInformationFile(h, &iosb, &fai, sizeof(fai), FileAllInformation);

    if (Status != STATUS_SUCCESS && Status != STATUS_BUFFER_OVERFLOW)
        throw ntstatus_error(Status);

    varbuf<FILE_ALL_INFORMATION> ret;

    ret.buf.resize(offsetof(FILE_ALL_INFORMATION, NameInformation.FileName) + fai.NameInformation.FileNameLength);

    auto& fai2 = *reinterpret_cast<FILE_ALL_INFORMATION*>(ret.buf.data());

    fai2.NameInformation.FileNameLength = ret.buf.size() - offsetof(FILE_ALL_INFORMATION, NameInformation.FileName);

    Status = NtQueryInformationFile(h, &iosb, &fai2, ret.buf.size(), FileAllInformation);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    if (iosb.Information != ret.buf.size())
        throw formatted_error("iosb.Information was {}, expected {}", iosb.Information, ret.buf.size());

    return ret;
}

template<typename T>
static void check_ea_dirent(const u16string& dir, u16string_view name, uint32_t exp_size) {
    auto items = query_dir<T>(dir, name);

    if (items.size() != 1)
        throw formatted_error("{} entries returned, expected 1.", items.size());

    auto& fdi = *static_cast<const T*>(items.front());

    if (fdi.FileNameLength != name.size() * sizeof(char16_t))
        throw formatted_error("FileNameLength was {}, expected {}.", fdi.FileNameLength, name.size() * sizeof(char16_t));

    if (name != u16string_view((char16_t*)fdi.FileName, fdi.FileNameLength / sizeof(char16_t)))
        throw runtime_error("FileName did not match.");

    if (fdi.EaSize != exp_size)
        throw formatted_error("EaSize was {}, expected {}", fdi.EaSize, exp_size);
}

void test_ea(const u16string& dir) {
    unique_handle h;

    test("Create file", [&]() {
        h = create_file(dir + u"\\ea1", FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES, 0, 0,
                        FILE_CREATE, 0, FILE_CREATED);
    });

    if (h) {
        static const string_view ea_name = "hello", ea_value = "world";

        test("Read EA", [&]() {
            exp_status([&]() {
                read_ea(h.get());
            }, STATUS_NO_EAS_ON_FILE);
        });

        test("Write EA", [&]() {
            write_ea(h.get(), ea_name, ea_value);
        });

        test("Read EA", [&]() {
            auto items = read_ea(h.get());

            if (items.size() != 1)
                throw formatted_error("{} entries returned, expected 1", items.size());

            auto& ffeai = *static_cast<FILE_FULL_EA_INFORMATION*>(items.front());

            if (ffeai.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai.Flags);

            auto name = string_view(ffeai.EaName, ffeai.EaNameLength);

            if (name != "HELLO") // gets capitalized
                throw formatted_error("EA name was \"{}\", expected \"HELLO\"", name);

            auto value = string_view(ffeai.EaName + ffeai.EaNameLength + 1, ffeai.EaValueLength);

            if (value != ea_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value, ea_value);
        });

        auto exp_size = ea_size(ea_name, ea_value);

        test("Query FileEaInformation", [&]() {
            auto feai = query_information<FILE_EA_INFORMATION>(h.get());

            if (feai.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", feai.EaSize, exp_size);
        });

        test("Query FileAllInformation", [&]() {
            auto buf = query_all_information(h.get());
            auto& fai = *static_cast<FILE_ALL_INFORMATION*>(buf);

            if (fai.EaInformation.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", fai.EaInformation.EaSize, exp_size);
        });

        h.reset();

        // EaSize in dirents not updated until file closed?
        test("Check directory entry (FILE_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\ea1", FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES, 0, 0,
                        FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        static const string_view ea1_name = "HELLO", ea1_value = "world";
        static const string_view ea2_name = "fOo", ea2_value = "bar";

        test("Add second EA", [&]() {
            write_ea(h.get(), ea2_name, ea2_value);
        });

        test("Read EAs", [&]() {
            auto items = read_ea(h.get());

            if (items.size() != 2)
                throw formatted_error("{} entries returned, expected 2", items.size());

            auto& ffeai1 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[0]);

            if (ffeai1.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai1.Flags);

            auto name1 = string_view(ffeai1.EaName, ffeai1.EaNameLength);

            if (name1 != ea1_name)
                throw formatted_error("EA name was \"{}\", expected \"{}\"", name1, ea1_name);

            auto value1 = string_view(ffeai1.EaName + ffeai1.EaNameLength + 1, ffeai1.EaValueLength);

            if (value1 != ea1_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value1, ea1_value);

            auto& ffeai2 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[1]);

            if (ffeai2.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai2.Flags);

            auto name2 = string_view(ffeai2.EaName, ffeai2.EaNameLength);

            if (name2 != "FOO")
                throw formatted_error("EA name was \"{}\", expected \"FOO\"", name2);

            auto value2 = string_view(ffeai2.EaName + ffeai2.EaNameLength + 1, ffeai2.EaValueLength);

            if (value2 != ea2_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value2, ea2_value);
        });

        auto exp_size = ea_size(ea1_name, ea1_value) + ea_size(ea2_name, ea2_value);

        test("Query FileEaInformation", [&]() {
            auto feai = query_information<FILE_EA_INFORMATION>(h.get());

            if (feai.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", feai.EaSize, exp_size);
        });

        test("Query FileAllInformation", [&]() {
            auto buf = query_all_information(h.get());
            auto& fai = *static_cast<FILE_ALL_INFORMATION*>(buf);

            if (fai.EaInformation.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", fai.EaInformation.EaSize, exp_size);
        });

        h.reset();

        test("Check directory entry (FILE_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\ea1", FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES, 0, 0,
                        FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        static const string_view ea1_name = "FOO", ea1_value = "bar";
        static const string_view ea2_name = "HeLlO", ea2_value = "baz";

        test("Replace first EA", [&]() {
            write_ea(h.get(), ea2_name, ea2_value);
        });

        test("Read EAs", [&]() {
            auto items = read_ea(h.get());

            if (items.size() != 2)
                throw formatted_error("{} entries returned, expected 2", items.size());

            auto& ffeai1 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[0]);

            if (ffeai1.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai1.Flags);

            auto name1 = string_view(ffeai1.EaName, ffeai1.EaNameLength);

            if (name1 != ea1_name)
                throw formatted_error("EA name was \"{}\", expected \"{}\"", name1, ea1_name);

            auto value1 = string_view(ffeai1.EaName + ffeai1.EaNameLength + 1, ffeai1.EaValueLength);

            if (value1 != ea1_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value1, ea1_value);

            auto& ffeai2 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[1]);

            if (ffeai2.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai2.Flags);

            auto name2 = string_view(ffeai2.EaName, ffeai2.EaNameLength);

            if (name2 != "HELLO")
                throw formatted_error("EA name was \"{}\", expected \"HELLO\"", name2);

            auto value2 = string_view(ffeai2.EaName + ffeai2.EaNameLength + 1, ffeai2.EaValueLength);

            if (value2 != ea2_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value2, ea2_value);
        });

        auto exp_size = ea_size(ea1_name, ea1_value) + ea_size(ea2_name, ea2_value);

        test("Query FileEaInformation", [&]() {
            auto feai = query_information<FILE_EA_INFORMATION>(h.get());

            if (feai.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", feai.EaSize, exp_size);
        });

        test("Query FileAllInformation", [&]() {
            auto buf = query_all_information(h.get());
            auto& fai = *static_cast<FILE_ALL_INFORMATION*>(buf);

            if (fai.EaInformation.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", fai.EaInformation.EaSize, exp_size);
        });

        h.reset();

        test("Check directory entry (FILE_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\ea1", FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES, 0, 0,
                        FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        static const string_view ea1_name = "FOO", ea1_value = "bar";
        static const string_view ea2_name = "HELlo";

        test("Delete first EA", [&]() {
            write_ea(h.get(), ea2_name, "");
        });

        test("Read EAs", [&]() {
            auto items = read_ea(h.get());

            if (items.size() != 1)
                throw formatted_error("{} entries returned, expected 1", items.size());

            auto& ffeai1 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[0]);

            if (ffeai1.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai1.Flags);

            auto name1 = string_view(ffeai1.EaName, ffeai1.EaNameLength);

            if (name1 != ea1_name)
                throw formatted_error("EA name was \"{}\", expected \"{}\"", name1, ea1_name);

            auto value1 = string_view(ffeai1.EaName + ffeai1.EaNameLength + 1, ffeai1.EaValueLength);

            if (value1 != ea1_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value1, ea1_value);
        });

        auto exp_size = ea_size(ea1_name, ea1_value);

        test("Query FileEaInformation", [&]() {
            auto feai = query_information<FILE_EA_INFORMATION>(h.get());

            if (feai.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", feai.EaSize, exp_size);
        });

        test("Query FileAllInformation", [&]() {
            auto buf = query_all_information(h.get());
            auto& fai = *static_cast<FILE_ALL_INFORMATION*>(buf);

            if (fai.EaInformation.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", fai.EaInformation.EaSize, exp_size);
        });

        h.reset();

        test("Check directory entry (FILE_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\ea1", FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES, 0, 0,
                        FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        static const string_view ea1_name = "foo";

        test("Delete second EA", [&]() {
            write_ea(h.get(), ea1_name, "");
        });

        test("Read EAs", [&]() {
            exp_status([&]() {
                read_ea(h.get());
            }, STATUS_NO_EAS_ON_FILE);
        });

        uint32_t exp_size = 0;

        test("Query FileEaInformation", [&]() {
            auto feai = query_information<FILE_EA_INFORMATION>(h.get());

            if (feai.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", feai.EaSize, exp_size);
        });

        test("Query FileAllInformation", [&]() {
            auto buf = query_all_information(h.get());
            auto& fai = *static_cast<FILE_ALL_INFORMATION*>(buf);

            if (fai.EaInformation.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", fai.EaInformation.EaSize, exp_size);
        });

        h.reset();

        test("Check directory entry (FILE_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_FULL_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(dir, u"ea1", exp_size);
        });
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\ea2", FILE_READ_EA | FILE_WRITE_EA | FILE_READ_ATTRIBUTES, 0, 0,
                        FILE_CREATE, 0, FILE_CREATED);
    });

    if (h) {
        static const string_view ea1_name = "qux", ea1_value = "xyzzy";
        static const string_view ea2_name = "y2", ea2_value = "plugh";

        test("Add two EAs", [&]() {
            write_eas(h.get(), array{ pair(ea1_name, ea1_value), pair(ea2_name, ea2_value) });
        });

        test("Read EAs", [&]() {
            auto items = read_ea(h.get());

            if (items.size() != 2)
                throw formatted_error("{} entries returned, expected 2", items.size());

            auto& ffeai1 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[0]);

            if (ffeai1.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai1.Flags);

            auto name1 = string_view(ffeai1.EaName, ffeai1.EaNameLength);

            if (name1 != "QUX")
                throw formatted_error("EA name was \"{}\", expected \"QUX\"", name1);

            auto value1 = string_view(ffeai1.EaName + ffeai1.EaNameLength + 1, ffeai1.EaValueLength);

            if (value1 != ea1_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value1, ea1_value);

            auto& ffeai2 = *static_cast<FILE_FULL_EA_INFORMATION*>(items[1]);

            if (ffeai2.Flags != 0)
                throw formatted_error("Flags was {:x}, expected 0", ffeai2.Flags);

            auto name2 = string_view(ffeai2.EaName, ffeai2.EaNameLength);

            if (name2 != "Y2")
                throw formatted_error("EA name was \"{}\", expected \"Y2\"", name2);

            auto value2 = string_view(ffeai2.EaName + ffeai2.EaNameLength + 1, ffeai2.EaValueLength);

            if (value2 != ea2_value)
                throw formatted_error("EA value was \"{}\", expected \"{}\"", value2, ea2_value);
        });

        uint32_t exp_size = ea_size(ea1_name, ea1_value) + ea_size(ea2_name, ea2_value);

        test("Query FileEaInformation", [&]() {
            auto feai = query_information<FILE_EA_INFORMATION>(h.get());

            if (feai.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", feai.EaSize, exp_size);
        });

        test("Query FileAllInformation", [&]() {
            auto buf = query_all_information(h.get());
            auto& fai = *static_cast<FILE_ALL_INFORMATION*>(buf);

            if (fai.EaInformation.EaSize != exp_size)
                throw formatted_error("EaSize was {}, expected {}", fai.EaInformation.EaSize, exp_size);
        });

        h.reset();

        test("Check directory entry (FILE_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_FULL_DIR_INFORMATION>(dir, u"ea2", exp_size);
        });

        test("Check directory entry (FILE_ID_FULL_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_FULL_DIR_INFORMATION>(dir, u"ea2", exp_size);
        });

        test("Check directory entry (FILE_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_BOTH_DIR_INFORMATION>(dir, u"ea2", exp_size);
        });

        test("Check directory entry (FILE_ID_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_BOTH_DIR_INFORMATION>(dir, u"ea2", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_DIR_INFORMATION>(dir, u"ea2", exp_size);
        });

        test("Check directory entry (FILE_ID_EXTD_BOTH_DIR_INFORMATION)", [&]() {
            check_ea_dirent<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(dir, u"ea2", exp_size);
        });
    }

    // FIXME - creating files with EAs
    // FIXME - EAs on directories?
    // FIXME - filter on NtQueryEaFile
    // FIXME - FILE_WRITE_EA and FILE_READ_EA
    // FIXME - FILE_NEED_EA
    // FIXME - FILE_NO_EA_KNOWLEDGE
}
