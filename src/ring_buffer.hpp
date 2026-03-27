// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <array>
#include <cstddef>

// 固定長リングバッファ
//
// 最新 N サンプルを保持する固定長バッファ。
// push で末尾に追加し、古いデータは自動的に上書きされる。
// at(0) が最古、at(size-1) が最新。
template<typename T, std::size_t N>
class RingBuffer {
public:
    RingBuffer() { data_.fill(T{}); }

    // 値を追加する（古い値を上書き）
    void push(T val) {
        data_[head_] = val;
        head_ = (head_ + 1) % N;
        if (count_ < N) ++count_;
    }

    // インデックスアクセス（0 = 最古、count-1 = 最新）
    T at(std::size_t i) const {
        return data_[(head_ - count_ + i + N) % N];
    }

    // 直近 n サンプルの平均値を返す（n=0 または空なら T{} を返す）
    T average(std::size_t n) const {
        if (count_ == 0 || n == 0) return T{};
        n = (n < count_) ? n : count_;
        T sum = T{};
        for (std::size_t i = count_ - n; i < count_; i++)
            sum += at(i);
        return sum / static_cast<T>(n);
    }

    std::size_t size()     const { return count_; }
    std::size_t capacity() const { return N; }
    bool        empty()    const { return count_ == 0; }

private:
    std::array<T, N> data_;
    std::size_t head_  = 0;
    std::size_t count_ = 0;
};
