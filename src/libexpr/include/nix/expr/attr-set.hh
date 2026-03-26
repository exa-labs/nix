#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"

#include <boost/container/static_vector.hpp>
#include <boost/iterator/function_output_iterator.hpp>

#include <algorithm>
#include <functional>
#include <ranges>
#include <optional>

namespace nix {

class EvalMemory;
struct Value;

/**
 * Map one attribute name to its value.
 */
struct Attr
{
    /* the placement of `name` and `pos` in this struct is important.
       both of them are uint32 wrappers, they are next to each other
       to make sure that Attr has no padding on 64 bit machines. that
       way we keep Attr size at two words with no wasted space. */
    Symbol name;
    PosIdx pos;
    Value * value = nullptr;
    Attr(Symbol name, Value * value, PosIdx pos = noPos)
        : name(name)
        , pos(pos)
        , value(value) {};
    Attr() {};

    auto operator<=>(const Attr & a) const
    {
        return name <=> a.name;
    }
};

static_assert(
    sizeof(Attr) == 2 * sizeof(uint32_t) + sizeof(Value *),
    "performance of the evaluator is highly sensitive to the size of Attr. "
    "avoid introducing any padding into Attr if at all possible, and do not "
    "introduce new fields that need not be present for almost every instance.");

/**
 * Bindings contains all the attributes of an attribute set. It is defined
 * by its size and its capacity, the capacity being the number of Attr
 * elements allocated after this structure, while the size corresponds to
 * the number of elements already inserted in this structure.
 *
 * Bindings can be efficiently `//`-composed into an intrusive linked list of "layers"
 * that saves on copies and allocations. Each lookup (@see Bindings::get) traverses
 * this linked list until a matching attribute is found (thus overlays earlier in
 * the list take precedence). For iteration over the whole Bindings, an on-the-fly
 * k-way merge is performed by Bindings::iterator class.
 */
class Bindings
{
public:
    using size_type = uint32_t;

    PosIdx pos;

    /**
     * An instance of bindings objects with 0 attributes.
     * This object must never be modified.
     */
    static Bindings emptyBindings;

private:
    /**
     * Number of attributes in the attrs FAM (Flexible Array Member).
     */
    size_type numAttrs = 0;

    /**
     * Number of attributes with unique names in the layer chain.
     *
     * This is the *real* user-facing size of bindings, whereas @ref numAttrs is
     * an implementation detail of the data structure.
     */
    size_type numAttrsInChain = 0;

    /**
     * Length of the layers list.
     */
    uint32_t numLayers = 1;

    /**
     * Bindings that this attrset is "layered" on top of.
     */
    const Bindings * baseLayer = nullptr;

    /**
     * Flexible array member of attributes.
     */
    Attr attrs[0];

    Bindings() = default;
    Bindings(const Bindings &) = delete;
    Bindings(Bindings &&) = delete;
    Bindings & operator=(const Bindings &) = delete;
    Bindings & operator=(Bindings &&) = delete;

    ~Bindings() = default;

    friend class BindingsBuilder;

    /**
     * Maximum length of the Bindings layer chains.
     */
    static constexpr unsigned maxLayers = 8;

public:
    size_type size() const
    {
        return numAttrsInChain;
    }

    bool empty() const
    {
        return size() == 0;
    }

    class iterator
    {
    public:
        using value_type = Attr;
        using pointer = const value_type *;
        using reference = const value_type &;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        friend class Bindings;

    private:
        struct BindingsCursor
        {
            /**
             * Attr that the cursor currently points to.
             */
            pointer current;

            /**
             * One past the end pointer to the contiguous buffer of Attrs.
             */
            pointer end;

            /**
             * Priority of the value. Lesser values have more priority (i.e. they override
             * attributes that appear later in the linked list of Bindings).
             */
            uint32_t priority;

            pointer operator->() const noexcept
            {
                return current;
            }

            reference get() const noexcept
            {
                return *current;
            }

            bool empty() const noexcept
            {
                return current == end;
            }

            void increment() noexcept
            {
                ++current;
            }

            void consume(Symbol name) noexcept
            {
                while (!empty() && current->name <= name)
                    ++current;
            }

            GENERATE_CMP(BindingsCursor, me->current->name, me->priority)
        };

        using QueueStorageType = boost::container::static_vector<BindingsCursor, maxLayers>;

        /**
         * Comparator implementing the override priority / name ordering
         * for BindingsCursor.
         */
        static constexpr auto comp = std::greater<BindingsCursor>();

        /**
         * A priority queue used to implement an on-the-fly k-way merge.
         */
        QueueStorageType cursorHeap;

        /**
         * The attribute the iterator currently points to.
         */
        pointer current = nullptr;

        /**
         * Iteration mode:
         *   Single  – one layer, simple pointer walk (no merge needed)
         *   TwoWay  – exactly two layers, fast two-pointer merge
         *   MultiWay – 3+ layers, heap-based k-way merge
         *
         * The two-way fast path eliminates all heap operations for the most
         * common layered case (a single // merge), which callgrind showed
         * as ~5 % of total instructions in real nixpkgs evaluations.
         */
        enum class Mode : uint8_t { Single, TwoWay, MultiWay };
        Mode mode = Mode::Single;

        void push(BindingsCursor cursor) noexcept
        {
            cursorHeap.push_back(cursor);
            std::ranges::push_heap(cursorHeap, comp);
        }

        [[nodiscard]] BindingsCursor pop() noexcept
        {
            std::ranges::pop_heap(cursorHeap, comp);
            auto cursor = cursorHeap.back();
            cursorHeap.pop_back();
            return cursor;
        }

        iterator & finished() noexcept
        {
            current = nullptr;
            return *this;
        }

        void next(BindingsCursor cursor) noexcept
        {
            current = &cursor.get();
            cursor.increment();

            if (!cursor.empty())
                push(cursor);
        }

        std::optional<BindingsCursor> consumeAllUntilCurrentName() noexcept
        {
            auto cursor = pop();
            Symbol lastHandledName = current->name;

            while (cursor->name <= lastHandledName) {
                cursor.consume(lastHandledName);
                if (!cursor.empty())
                    push(cursor);

                if (cursorHeap.empty())
                    return std::nullopt;

                cursor = pop();
            }

            return cursor;
        }

        /* --- Two-way merge helpers --- */

        /**
         * Find the initial element from two cursors stored at
         * cursorHeap[0] (overlay, higher priority) and cursorHeap[1] (base).
         */
        void initTwoWay() noexcept
        {
            auto & overlay = cursorHeap[0];
            auto & base = cursorHeap[1];

            if (overlay.empty() && base.empty())
                return;
            if (overlay.empty()) {
                current = base.current;
                return;
            }
            if (base.empty()) {
                current = overlay.current;
                return;
            }

            if (overlay.current->name <= base.current->name) {
                current = overlay.current;
                /* If both layers define the same attr, skip the base copy. */
                if (overlay.current->name == base.current->name)
                    base.increment();
            } else {
                current = base.current;
            }
        }

        /**
         * Advance to the next element in a two-layer merge.
         * Avoids all heap operations – just two pointer comparisons.
         */
        iterator & advanceTwoWay() noexcept
        {
            auto & overlay = cursorHeap[0];
            auto & base = cursorHeap[1];
            Symbol currentName = current->name;

            /* Advance whichever cursor(s) produced the current element. */
            if (!overlay.empty() && overlay.current->name == currentName)
                overlay.increment();
            if (!base.empty() && base.current->name == currentName)
                base.increment();

            bool has0 = !overlay.empty();
            bool has1 = !base.empty();

            if (!has0 && !has1)
                return finished();
            if (!has0) {
                current = base.current;
                return *this;
            }
            if (!has1) {
                current = overlay.current;
                return *this;
            }

            /* Both cursors have elements – pick the smaller name,
               preferring the overlay on ties (it has higher priority). */
            if (overlay.current->name < base.current->name) {
                current = overlay.current;
            } else if (overlay.current->name > base.current->name) {
                current = base.current;
            } else {
                /* Equal names – overlay wins, skip base duplicate. */
                current = overlay.current;
                base.increment();
            }
            return *this;
        }

        /* --- Constructor --- */

        explicit iterator(const Bindings & attrs) noexcept
        {
            if (!attrs.baseLayer) {
                /* Single layer – no merge needed. */
                mode = Mode::Single;
                if (attrs.empty())
                    return;

                current = attrs.attrs;
                cursorHeap.push_back(BindingsCursor{
                    .current = attrs.attrs,
                    .end = attrs.attrs + attrs.numAttrs,
                    .priority = 0,
                });
                return;
            }

            if (attrs.numLayers == 2) {
                /* Two layers – fast two-pointer merge (no heap). */
                mode = Mode::TwoWay;

                /* cursorHeap[0] = overlay (higher priority, the front layer). */
                cursorHeap.push_back(BindingsCursor{
                    .current = attrs.attrs,
                    .end = attrs.attrs + attrs.numAttrs,
                    .priority = 0,
                });
                /* cursorHeap[1] = base. */
                cursorHeap.push_back(BindingsCursor{
                    .current = attrs.baseLayer->attrs,
                    .end = attrs.baseLayer->attrs + attrs.baseLayer->numAttrs,
                    .priority = 1,
                });

                initTwoWay();
                return;
            }

            /* 3+ layers – heap-based k-way merge. */
            mode = Mode::MultiWay;

            const Bindings * layer = &attrs;
            unsigned priority = 0;
            while (layer) {
                if (layer->numAttrs != 0) {
                    cursorHeap.push_back(BindingsCursor{
                        .current = layer->attrs,
                        .end = layer->attrs + layer->numAttrs,
                        .priority = priority,
                    });
                }
                priority++;
                layer = layer->baseLayer;
            }

            if (cursorHeap.empty())
                return;

            /* Build the heap once instead of rebuilding after each push. */
            std::ranges::make_heap(cursorHeap, comp);
            next(pop());
        }

    public:
        iterator() = default;

        reference operator*() const noexcept
        {
            return *current;
        }

        pointer operator->() const noexcept
        {
            return current;
        }

        iterator & operator++() noexcept
        {
            switch (mode) {
            case Mode::Single:
                ++current;
                if (current == cursorHeap.front().end)
                    return finished();
                return *this;

            case Mode::TwoWay:
                return advanceTwoWay();

            case Mode::MultiWay:
                if (cursorHeap.empty())
                    return finished();
                {
                    auto cursor = consumeAllUntilCurrentName();
                    if (!cursor)
                        return finished();
                    next(*cursor);
                }
                return *this;
            }

            __builtin_unreachable();
        }

        iterator operator++(int) noexcept
        {
            iterator tmp = *this;
            ++*this;
            return tmp;
        }

        bool operator==(const iterator & rhs) const noexcept
        {
            return current == rhs.current;
        }
    };

    using const_iterator = iterator;

    void push_back(const Attr & attr)
    {
        attrs[numAttrs++] = attr;
        numAttrsInChain = numAttrs;
    }

    /**
     * Get attribute by name or nullptr if no such attribute exists.
     *
     * For small chunks (≤ 8 elements), uses linear scan instead of binary
     * search. Linear scan has better cache behavior and avoids branch
     * mispredictions from the binary search pivot comparisons, making it
     * faster for the small attribute sets that dominate in practice.
     */
    const Attr * get(Symbol name) const noexcept
    {
        static constexpr size_type linearScanThreshold = 8;

        const Bindings * currentChunk = this;
        while (currentChunk) {
            auto first = currentChunk->attrs;
            auto n = currentChunk->numAttrs;

            if (n <= linearScanThreshold) {
                /* Linear scan for small chunks. Since attrs are sorted,
                   we can stop early when we pass the target name. */
                for (size_type i = 0; i < n; ++i) {
                    if (first[i].name == name)
                        return &first[i];
                    if (first[i].name > name)
                        break;
                }
            } else {
                /* Binary search for larger chunks. */
                auto last = first + n;
                auto key = Attr{name, nullptr};
                const Attr * i = std::lower_bound(first, last, key);
                if (i != last && i->name == name)
                    return i;
            }

            currentChunk = currentChunk->baseLayer;
        }

        return nullptr;
    }

    /**
     * Check if the layer chain is full.
     */
    bool isLayerListFull() const noexcept
    {
        return numLayers == Bindings::maxLayers;
    }

    /**
     * Test if the length of the linked list of layers is greater than 1.
     */
    bool isLayered() const noexcept
    {
        return numLayers > 1;
    }

    const_iterator begin() const
    {
        return const_iterator(*this);
    }

    const_iterator end() const
    {
        return const_iterator();
    }

    Attr & operator[](size_type pos)
    {
        if (isLayered()) [[unlikely]]
            unreachable();
        return attrs[pos];
    }

    const Attr & operator[](size_type pos) const
    {
        if (isLayered()) [[unlikely]]
            unreachable();
        return attrs[pos];
    }

    void sort();

    /**
     * Returns the attributes in lexicographically sorted order.
     */
    std::vector<const Attr *> lexicographicOrder(const SymbolTable & symbols) const
    {
        std::vector<const Attr *> res;
        res.reserve(size());
        std::ranges::transform(*this, std::back_inserter(res), [](const Attr & a) { return &a; });
        std::ranges::sort(res, [&](const Attr * a, const Attr * b) {
            std::string_view sa = symbols[a->name], sb = symbols[b->name];
            return sa < sb;
        });
        return res;
    }

    friend class EvalMemory;
};

static_assert(std::forward_iterator<Bindings::iterator>);
static_assert(std::ranges::forward_range<Bindings>);

/**
 * A wrapper around Bindings that ensures that its always in sorted
 * order at the end. The only way to consume a BindingsBuilder is to
 * call finish(), which sorts the bindings.
 */
class BindingsBuilder final
{
public:
    // needed by std::back_inserter
    using value_type = Attr;
    using size_type = Bindings::size_type;

private:
    Bindings * bindings;
    Bindings::size_type capacity_;

    friend class EvalMemory;

    BindingsBuilder(EvalMemory & mem, SymbolTable & symbols, Bindings * bindings, size_type capacity)
        : bindings(bindings)
        , capacity_(capacity)
        , mem(mem)
        , symbols(symbols)
    {
    }

    bool hasBaseLayer() const noexcept
    {
        return bindings->baseLayer;
    }

    /**
     * If the bindings gets "layered" on top of another we need to recalculate
     * the number of unique attributes in the chain.
     *
     * This is done by either iterating over the base "layer" and the newly added
     * attributes and counting duplicates. If the base "layer" is big this approach
     * is inefficient and we fall back to doing per-element binary search in the base
     * "layer".
     */
    void finishSizeIfNecessary()
    {
        if (!hasBaseLayer())
            return;

        auto & base = *bindings->baseLayer;
        auto attrs = std::span(bindings->attrs, bindings->numAttrs);

        Bindings::size_type duplicates = 0;

        /* If the base bindings is smaller than the newly added attributes
           iterate using std::set_intersection to run in O(|base| + |attrs|) =
           O(|attrs|). Otherwise use an O(|attrs| * log(|base|)) per-attr binary
           search to check for duplicates. Note that if we are in this code path then
           |attrs| <= bindingsUpdateLayerRhsSizeThreshold, which 16 by default. We are
           optimizing for the case when a small attribute set gets "layered" on top of
           a much larger one. When attrsets are already small it's fine to do a linear
           scan, but we should avoid expensive iterations over large "base" attrsets. */
        if (attrs.size() > base.size()) {
            std::set_intersection(
                base.begin(),
                base.end(),
                attrs.begin(),
                attrs.end(),
                boost::make_function_output_iterator([&]([[maybe_unused]] auto && _) { ++duplicates; }));
        } else {
            for (const auto & attr : attrs) {
                if (base.get(attr.name))
                    ++duplicates;
            }
        }

        bindings->numAttrsInChain = base.numAttrsInChain + attrs.size() - duplicates;
    }

public:
    std::reference_wrapper<EvalMemory> mem;
    std::reference_wrapper<SymbolTable> symbols;

    void insert(Symbol name, Value * value, PosIdx pos = noPos)
    {
        insert(Attr(name, value, pos));
    }

    void insert(const Attr & attr)
    {
        push_back(attr);
    }

    void push_back(const Attr & attr)
    {
        assert(bindings->numAttrs < capacity_);
        bindings->push_back(attr);
    }

    /**
     * "Layer" the newly constructured Bindings on top of another attribute set.
     *
     * This effectively performs an attribute set merge, while giving preference
     * to attributes from the newly constructed Bindings in case of duplicate attribute
     * names.
     *
     * This operation amortizes the need to copy over all attributes and allows
     * for efficient implementation of attribute set merges (ExprOpUpdate::eval).
     */
    void layerOnTopOf(const Bindings & base) noexcept
    {
        bindings->baseLayer = &base;
        bindings->numLayers = base.numLayers + 1;
    }

    Value & alloc(Symbol name, PosIdx pos = noPos);

    Value & alloc(std::string_view name, PosIdx pos = noPos);

    Bindings * finish()
    {
        bindings->sort();
        finishSizeIfNecessary();
        return bindings;
    }

    Bindings * alreadySorted()
    {
        finishSizeIfNecessary();
        return bindings;
    }

    size_t capacity() const noexcept
    {
        return capacity_;
    }

    void grow(BindingsBuilder newBindings)
    {
        for (auto & i : *bindings)
            newBindings.push_back(i);
        std::swap(*this, newBindings);
    }

    friend struct ExprAttrs;
};

} // namespace nix
