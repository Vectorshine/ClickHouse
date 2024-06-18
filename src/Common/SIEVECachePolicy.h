#pragma once

#include <Common/ICachePolicy.h>

#include <list>
#include <unordered_map>

namespace DB
{
/// Cache policy SIEVE evicts entries which are not used for a long time. Also see cache policy SLRU for reference.
/// WeightFunction is a functor that takes Mapped as a parameter and returns "weight" (approximate size) of that value.
/// Cache starts to evict entries when their total weight exceeds max_size_in_bytes.
/// Value weight should not change after insertion.
/// To work with the thread-safe implementation of this class use a class "CacheBase" with first parameter "SIEVE"
/// and next parameters in the same order as in the constructor of the current class.
/// For more details, see https://junchengyang.com/publication/nsdi24-SIEVE.pdf
template <typename Key, typename Mapped, typename HashFunction = std::hash<Key>, typename WeightFunction = EqualWeightFunction<Mapped>>
class SIEVECachePolicy : public ICachePolicy<Key, Mapped, HashFunction, WeightFunction>
{
public:
    using Base = ICachePolicy<Key, Mapped, HashFunction, WeightFunction>;
    using typename Base::MappedPtr;
    using typename Base::KeyMapped;
    using typename Base::OnWeightLossFunction;

    /** Initialize SIEVECachePolicy with max_size_in_bytes and max_count.
     *  max_size_in_bytes == 0 means the cache accepts no entries.
      * max_count == 0 means no elements size restrictions.
      */
    SIEVECachePolicy(size_t max_size_in_bytes_, size_t max_count_, OnWeightLossFunction on_weight_loss_function_)
        : Base(std::make_unique<NoCachePolicyUserQuota>())
        , max_size_in_bytes(max_size_in_bytes_)
        , max_count(max_count_)
        , on_weight_loss_function(on_weight_loss_function_)
    {
        hand = queue.begin();
    }

    size_t sizeInBytes() const override
    {
        return current_size_in_bytes;
    }

    size_t count() const override
    {
        return cells.size();
    }

    size_t maxSizeInBytes() const override
    {
        return max_size_in_bytes;
    }

    void setMaxCount(size_t max_count_) override
    {
        max_count = max_count_;
        removeOverflow();
    }

    void setMaxSizeInBytes(size_t max_size_in_bytes_) override
    {
        max_size_in_bytes = max_size_in_bytes_;
        removeOverflow();
    }

    void clear() override
    {
        queue.clear();
        cells.clear();
        current_size_in_bytes = 0;
        hand = queue.begin();
    }

    void remove(const Key & key) override
    {
        auto it = cells.find(key);
        if (it == cells.end())
            return;
        auto & cell = it->second;
        current_size_in_bytes -= cell.size;
        if (hand == cell.queue_iterator)
            hand = std::next(cell.queue_iterator) == queue.end() ? queue.begin() : ++cell.queue_iterator;
        queue.erase(cell.queue_iterator);
        cells.erase(it);
    }

    MappedPtr get(const Key & key) override
    {
        auto it = cells.find(key);
        if (it == cells.end())
            return {};

        Cell & cell = it->second;
        cell.visited = true;

        return cell.value;
    }

    std::optional<KeyMapped> getWithKey(const Key & key) override
    {
        auto it = cells.find(key);
        if (it == cells.end())
            return std::nullopt;

        Cell & cell = it->second;
        cell.visited = true;

        return std::make_optional<KeyMapped>({it->first, cell.value});
    }

    void set(const Key & key, const MappedPtr & mapped) override
    {
        auto [it, inserted] = cells.emplace(std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple());

        Cell & cell = it->second;

        if (inserted)
        {
            try
            {
                cell.queue_iterator = queue.insert(queue.end(), key);
                cell.visited = 0;
            }
            catch (...)
            {
                cells.erase(it);
                throw;
            }
        }
        else
        {
            current_size_in_bytes -= cell.size;
            cell.visited = true;
        }

        cell.value = mapped;
        cell.size = cell.value ? weight_function(*cell.value) : 0;
        current_size_in_bytes += cell.size;

        removeOverflow();
    }

    std::vector<KeyMapped> dump() const override
    {
        std::vector<KeyMapped> res;
        for (const auto & [key, cell] : cells)
            res.push_back({key, cell.value});
        return res;
    }

private:
    using SIEVEQueue = std::list<Key>;
    using SIEVEQueueIterator = typename SIEVEQueue::iterator;

    SIEVEQueue queue;
    typename SIEVEQueue::iterator hand;
    const size_t up = 1;
    struct Cell
    {
        MappedPtr value;
        size_t size;
        bool visited;
        SIEVEQueueIterator queue_iterator;
    };

    using Cells = std::unordered_map<Key, Cell, HashFunction>;

    Cells cells;

    /// Total weight of values.
    size_t current_size_in_bytes = 0;
    size_t max_size_in_bytes;
    size_t max_count;

    WeightFunction weight_function;
    OnWeightLossFunction on_weight_loss_function;

    void removeOverflow()
    {
        size_t current_weight_lost = 0;
        size_t queue_size = cells.size();

        while ((current_size_in_bytes > max_size_in_bytes || (max_count != 0 && queue_size > max_count)) && (queue_size > 0))
        {
            if (hand == queue.end())
            {
                // Reset hand to the start if we reach the end
                hand = queue.begin();
            }

            const Key & key = *hand;
            auto it = cells.find(key);

            if (it == cells.end())
            {
                std::terminate(); // Queue became inconsistent
            }

            auto & cell = it->second;

            if (cell.visited == false)
            {
                // Evict the current item
                current_size_in_bytes -= cell.size;
                current_weight_lost += cell.size;

                hand = queue.erase(hand);
                cells.erase(it);
                --queue_size;
            }
            else
            {
                // Mark the current hand as visited
                cell.visited = false;
                ++hand;
            }
        }

        on_weight_loss_function(current_weight_lost);

        if (current_size_in_bytes > (1ull << 63))
            std::terminate(); // Queue became inconsistent
    }
};

}
