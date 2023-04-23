#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <istream>
#include <limits>
#include <ostream>

#include <prettyprint.hpp>
#include <stdexcept>
#include <vector>

struct Entry
{
    constexpr static size_t entry_size = sizeof(int) + 30 + 20;

    int key;
    char nombre[30]{};
    char carrera[20]{};

    Entry()
        : key(0)
    {
    }

    Entry(int _key, std::string const& _nombre, std::string const& _carrera)
        : key{_key}
    {
        size_t nombre_n_copied = std::min(_nombre.size(), size_t(30));
        auto nombre_last_copied = std::copy(
            _nombre.begin(), std::next(_nombre.begin(), nombre_n_copied), (char*)nombre);
        std::fill(nombre_last_copied, std::end(nombre), '\0');

        size_t carrera_n_copied = std::min(_carrera.size(), size_t(20));
        auto carrera_last_copied = std::copy(
            _carrera.begin(), std::next(_carrera.begin(), carrera_n_copied), (char*)carrera);
        std::fill(carrera_last_copied, std::end(carrera), '\0');
    }

    void serialize(std::ostream& os) const
    {
        os.write(reinterpret_cast<char const*>(&key), sizeof(key));
        os.write(reinterpret_cast<char const*>(nombre), 30);
        os.write(reinterpret_cast<char const*>(carrera), 20);
    }

    void deserialize(std::istream& in)
    {
        in.read(reinterpret_cast<char*>(&key), sizeof(key));
        in.read(reinterpret_cast<char*>(nombre), 30);
        in.read(reinterpret_cast<char*>(carrera), 20);
    }

    friend auto operator<<(std::ostream& os, Entry const& e) -> std::ostream&
    {
        os << "<Entry: " << e.key << ", " << e.nombre << ", " << e.carrera << ">";
        return os;
    }
};

template<>
struct std::hash<Entry>
{
    auto operator()(Entry const& e) const noexcept -> std::size_t
    {
        return std::hash<typeof(Entry::key)>{}(e.key);
    }
};

struct HashFile
{
    constexpr static size_t n_buckets = 5;

    std::fstream file;
    std::hash<Entry> hasher;

    struct Bucket
    {
        constexpr static size_t fb = 4;

        std::array<bool, fb> entries_mask{false}; // Corresponding index is true if active.
        std::array<Entry, fb> entries{};
        size_t overflow_pointer = overflow_no_overflow;

        constexpr static size_t overflow_no_overflow = std::numeric_limits<size_t>::max();

        constexpr static size_t bucket_size =
            fb * (sizeof(bool) + Entry::entry_size) + sizeof(overflow_pointer);

        void serialize(std::ostream& os)
        {
            // Smarter ways of packing bool values exist
            for (bool b : entries_mask)
            {
                os.write(reinterpret_cast<char const*>(&b), sizeof(b));
            }
            for (Entry const& e : entries)
            {
                e.serialize(os);
            }
            os.write(
                reinterpret_cast<char const*>(&overflow_pointer), sizeof(overflow_pointer));
        }
    };

    struct BucketView
    {
        size_t bucket_offset;
        std::fstream* file;

        void to_offset()
        {
            file->seekg(bucket_offset, std::ios::beg);
        }

        auto read_entries_mask() -> std::array<bool, Bucket::fb>
        {
            to_offset();

            std::array<bool, Bucket::fb> ret;
            for (size_t i = 0; i < Bucket::fb; i++)
            {
                file->read(reinterpret_cast<char*>(&ret[i]), sizeof(bool));
            }

            return ret;
        }

        bool read_entry_mask(size_t pos)
        {
            to_offset();

            file->seekg(pos * sizeof(bool), std::ios::cur);
            bool ret = 0;
            file->read(reinterpret_cast<char*>(&ret), sizeof(ret));
            return ret;
        }

        void set_entry_mask(size_t pos, bool entry_mask)
        {
            to_offset();

            file->seekp(pos * sizeof(bool), std::ios::cur);
            file->write(reinterpret_cast<char const*>(&entry_mask), sizeof(entry_mask));
        }

        auto read_entries() -> std::array<Entry, Bucket::fb>
        {
            to_offset();

            file->seekg(Bucket::fb * sizeof(bool), std::ios::cur);

            std::array<Entry, Bucket::fb> ret{};
            Entry temp{};
            for (size_t i = 0; i < Bucket::fb; i++)
            {
                temp.deserialize(*file);
                ret[i] = temp;
            }

            return ret;
        }

        auto read_entry(size_t pos) -> Entry
        {
            to_offset();

            file->seekg(Bucket::fb * sizeof(bool) + pos * Entry::entry_size, std::ios::cur);

            Entry ret{};
            ret.deserialize(*file);

            return ret;
        }

        void set_entry(size_t pos, Entry const& entry)
        {
            to_offset();

            file->seekg(Bucket::fb * sizeof(bool) + pos * Entry::entry_size, std::ios::cur);

            entry.serialize(*file);
        }

        auto read_overflow_pointer() -> size_t
        {
            to_offset();
            file->seekg(
                Bucket::fb * sizeof(bool) + Bucket::fb * Entry::entry_size, std::ios::cur);

            size_t ret = 0;
            file->read(reinterpret_cast<char*>(&ret), sizeof(ret));
            return ret;
        }

        void set_overflow_pointer(size_t overflow_pointer)
        {
            to_offset();
            file->seekp(
                Bucket::fb * sizeof(bool) + Bucket::fb * Entry::entry_size, std::ios::cur);

            file->write(
                reinterpret_cast<char const*>(&overflow_pointer), sizeof(overflow_pointer));
        }
    };

    HashFile(std::string const& file_name)
    {
        std::ofstream(file_name, std::ofstream::app | std::fstream::binary);
        file = std::fstream{file_name, std::ios::in | std::ios::out | std::ios::binary};

        // We assume the file is either empty or properly initialized

        file.seekg(0, std::ios::end);
        if (file.tellg() == 0)
        {
            Bucket default_bucket = Bucket{};
            for (size_t i = 0; i < n_buckets; i++)
            {
                default_bucket.serialize(file);
            }
        }
    }

    void add(Entry const& e)
    {
        // XXX: This results in a increasingly non-uniform distribution for larger n_buckets.
        size_t hash_pos = hasher(e) % n_buckets;

        size_t bucket_pos = hash_pos * Bucket::bucket_size;

        auto bv = BucketView{bucket_pos, &file};
        while (true)
        {
            size_t op = bv.read_overflow_pointer();
            if (op == Bucket::overflow_no_overflow)
            {
                break;
            }
            bv = BucketView{op * Bucket::bucket_size, &file};
        }

        auto ems = bv.read_entries_mask();
        auto found = std::find(ems.begin(), ems.end(), false);

        // All positions used, should create new overflow
        if (found == ems.end())
        {
            file.seekp(0, std::ios::end);
            size_t last_pos = file.tellp();
            size_t new_bucket_index = last_pos / Bucket::bucket_size;

            bv.set_overflow_pointer(new_bucket_index);

            file.seekp(0, std::ios::end);
            Bucket{}.serialize(file);

            auto new_bv = BucketView{last_pos, &file};
            new_bv.set_entry_mask(0, true);
            new_bv.set_entry(0, e);
        }
        else
        {
            size_t index = std::distance(ems.begin(), found);

            bv.set_entry_mask(index, true);
            bv.set_entry(index, e);
        }
    }

    auto find(int key) -> std::vector<Entry>
    {
        size_t index = std::hash<int>{}(key) % n_buckets;

        std::vector<Entry> ret;
        while (index != Bucket::overflow_no_overflow)
        {
            auto bv = BucketView{index * Bucket::bucket_size, &file};

            auto entries_mask = bv.read_entries_mask();

            for (size_t i = 0; i < entries_mask.size(); i++)
            {
                if (entries_mask[i])
                {
                    auto temp = bv.read_entry(i);
                    if (temp.key == key)
                    {
                        ret.emplace_back(std::move(temp));
                    }
                }
            }

            index = bv.read_overflow_pointer();
        }

        return ret;
    }
};

auto main() -> int
{
    HashFile hf("hf1.bin");

    hf.add({1, "Alvaro", "A1"});
    hf.add({1, "Alfredo", "A2"});
    hf.add({1, "Alejandro", "A3"});
    hf.add({1, "Alberto", "A4"});
    hf.add({1, "Alex", "A5"});
    hf.add({2, "Brazil", "B1"});
    hf.add({3, "Carlos", "C1"});
    hf.add({4, "Diego", "D1"});
    hf.add({4, "Don", "D2"});
    hf.add({5, "Enrique", "E1"});
    hf.add({6, "Federico", "F1"});

    std::cerr << hf.find(1) << "\n";
    std::cerr << hf.find(2) << "\n";
    std::cerr << hf.find(6) << "\n";

    return 0;
}
