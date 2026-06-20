// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "doctest.h"
#include "ring_buffer.hpp"

TEST_CASE("RingBuffer: 初期状態は空・容量 N") {
    RingBuffer<int, 5> rb;
    CHECK(rb.size() == 0);
    CHECK(rb.empty());
    CHECK(rb.capacity() == 5);
}

TEST_CASE("RingBuffer: push が N で頭打ち、最古→最新の並び") {
    RingBuffer<int, 3> rb;
    rb.push(1); rb.push(2); rb.push(3); rb.push(4); rb.push(5);
    CHECK(rb.size() == 3);
    CHECK_FALSE(rb.empty());
    CHECK(rb.at(0) == 3);  // 最古
    CHECK(rb.at(1) == 4);
    CHECK(rb.at(2) == 5);  // 最新
}

TEST_CASE("RingBuffer<float>: average の n クランプと境界") {
    RingBuffer<float, 5> rb;
    for (int i = 1; i <= 5; ++i) {
        rb.push(static_cast<float>(i));
    }
    CHECK(rb.average(3)   == doctest::Approx(4.0f));  // (3+4+5)/3
    CHECK(rb.average(5)   == doctest::Approx(3.0f));  // (1+2+3+4+5)/5
    CHECK(rb.average(100) == doctest::Approx(3.0f));  // count_ にクランプされ全平均
    CHECK(rb.average(0)   == 0.0f);                   // n=0 で T{}
}

TEST_CASE("RingBuffer<float>: 空のときの average は T{}") {
    RingBuffer<float, 5> rb;
    CHECK(rb.average(3) == 0.0f);
}

TEST_CASE("RingBuffer: at は容量を超えても循環参照しない（最新 N 個のみ保持）") {
    RingBuffer<int, 4> rb;
    for (int i = 0; i < 10; ++i) {
        rb.push(i);
    }
    CHECK(rb.size() == 4);
    CHECK(rb.at(0) == 6);
    CHECK(rb.at(1) == 7);
    CHECK(rb.at(2) == 8);
    CHECK(rb.at(3) == 9);
}
