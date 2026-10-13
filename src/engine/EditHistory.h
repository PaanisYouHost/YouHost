#pragma once

#include <cstddef>
#include <deque>
#include <utility>

namespace youhost
{

// Message-thread edit stack. It stores whatever the caller captured before a
// change. It does not know about takes, WAV files, or the audio callback.
template <typename Snapshot>
class EditHistory
{
public:
    static constexpr int kLimit = 50;

    void push(Snapshot before)
    {
        undo_.push_back(std::move(before));
        while (static_cast<int>(undo_.size()) > kLimit)
            undo_.pop_front();
        redo_.clear();
    }

    bool canUndo() const noexcept { return ! undo_.empty(); }
    bool canRedo() const noexcept { return ! redo_.empty(); }
    const Snapshot* undoTop() const noexcept { return undo_.empty() ? nullptr : &undo_.back(); }
    const Snapshot* redoTop() const noexcept { return redo_.empty() ? nullptr : &redo_.back(); }
    int undoDepth() const noexcept { return static_cast<int>(undo_.size()); }
    int redoDepth() const noexcept { return static_cast<int>(redo_.size()); }

    // `current` is the state being left. Returns false when there is nothing to do.
    bool undo(Snapshot current, Snapshot& restored)
    {
        if (undo_.empty())
            return false;
        redo_.push_back(std::move(current));
        restored = std::move(undo_.back());
        undo_.pop_back();
        return true;
    }

    bool redo(Snapshot current, Snapshot& restored)
    {
        if (redo_.empty())
            return false;
        undo_.push_back(std::move(current));
        while (static_cast<int>(undo_.size()) > kLimit)
            undo_.pop_front();
        restored = std::move(redo_.back());
        redo_.pop_back();
        return true;
    }

    void clear() noexcept
    {
        undo_.clear();
        redo_.clear();
    }

private:
    std::deque<Snapshot> undo_;
    std::deque<Snapshot> redo_;
};

} // namespace youhost
