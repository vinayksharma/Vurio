// QW-6 (backend_optimization_plan.md): the pair-comparison hot path was
// refactored (cached logger instead of a per-call global registry lookup).
// These tests pin computeEmbeddingSimilarity's semantics through that and any
// future hot-loop refactor (DF-1 will replace the byte-copies next).

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "media_processors/similarity/similarity_calculator.hpp"

using MediaDedup::SimilarityCalculator;

namespace
{
    std::vector<std::uint8_t> toBytes(const std::vector<float> &values)
    {
        std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    }
} // namespace

TEST(SimilarityCalculatorTest, IdenticalEmbeddingsScoreOne)
{
    std::vector<float> v(512, 0.5f);
    auto b = toBytes(v);
    EXPECT_NEAR(SimilarityCalculator::computeEmbeddingSimilarity(b, b, 512), 1.0, 1e-6);
}

TEST(SimilarityCalculatorTest, CosineIsScaleInvariant)
{
    std::vector<float> v1(512), v2(512);
    for (int i = 0; i < 512; ++i)
    {
        v1[i] = static_cast<float>(i % 7) - 3.0f;
        v2[i] = 2.5f * v1[i]; // same direction, different magnitude
    }
    EXPECT_NEAR(SimilarityCalculator::computeEmbeddingSimilarity(toBytes(v1), toBytes(v2), 512),
                1.0, 1e-6);
}

TEST(SimilarityCalculatorTest, OrthogonalEmbeddingsScoreZero)
{
    std::vector<float> v1(4, 0.0f), v2(4, 0.0f);
    v1[0] = 1.0f; // e0
    v2[1] = 1.0f; // e1
    EXPECT_NEAR(SimilarityCalculator::computeEmbeddingSimilarity(toBytes(v1), toBytes(v2), 4),
                0.0, 1e-6);
}

TEST(SimilarityCalculatorTest, OppositeEmbeddingsScoreMinusOne)
{
    std::vector<float> v1(64), v2(64);
    for (int i = 0; i < 64; ++i)
    {
        v1[i] = static_cast<float>(i + 1);
        v2[i] = -v1[i];
    }
    EXPECT_NEAR(SimilarityCalculator::computeEmbeddingSimilarity(toBytes(v1), toBytes(v2), 64),
                -1.0, 1e-6);
}

TEST(SimilarityCalculatorTest, DegenerateInputsScoreZero)
{
    std::vector<float> v(512, 0.25f);
    auto good = toBytes(v);

    // Empty input.
    EXPECT_EQ(SimilarityCalculator::computeEmbeddingSimilarity({}, good, 512), 0.0);
    EXPECT_EQ(SimilarityCalculator::computeEmbeddingSimilarity(good, {}, 512), 0.0);

    // Size mismatch vs the declared dimension.
    auto short_blob = toBytes(std::vector<float>(256, 0.25f));
    EXPECT_EQ(SimilarityCalculator::computeEmbeddingSimilarity(short_blob, good, 512), 0.0);

    // Zero-norm vector.
    auto zeros = toBytes(std::vector<float>(512, 0.0f));
    EXPECT_EQ(SimilarityCalculator::computeEmbeddingSimilarity(zeros, good, 512), 0.0);
}

// ---- DF-1: normalized dot-product fast path ------------------------------

TEST(SimilarityCalculatorTest, DotSimilarityMatchesLegacyCosine)
{
    // Two arbitrary (unnormalized) vectors: normalizing then taking the bare
    // dot product must agree with the legacy blob-based cosine.
    std::vector<float> v1(512), v2(512);
    for (int i = 0; i < 512; ++i)
    {
        v1[i] = 0.001f * static_cast<float>((i * 37) % 101) - 0.05f;
        v2[i] = 0.002f * static_cast<float>((i * 53) % 89) - 0.07f;
    }

    const double legacy = SimilarityCalculator::computeEmbeddingSimilarity(toBytes(v1), toBytes(v2), 512);

    std::vector<float> n1 = v1, n2 = v2;
    ASSERT_TRUE(SimilarityCalculator::l2NormalizeInPlace(n1.data(), 512));
    ASSERT_TRUE(SimilarityCalculator::l2NormalizeInPlace(n2.data(), 512));
    const double fast = SimilarityCalculator::dotSimilarity(n1.data(), n2.data(), 512);

    EXPECT_NEAR(fast, legacy, 1e-5);
}

TEST(SimilarityCalculatorTest, L2NormalizeProducesUnitNorm)
{
    std::vector<float> v(64, 3.0f);
    ASSERT_TRUE(SimilarityCalculator::l2NormalizeInPlace(v.data(), 64));
    double norm_sq = 0.0;
    for (float x : v)
        norm_sq += static_cast<double>(x) * x;
    EXPECT_NEAR(norm_sq, 1.0, 1e-6);
    // Self-similarity of a unit vector is exactly 1 (post-clamp).
    EXPECT_NEAR(SimilarityCalculator::dotSimilarity(v.data(), v.data(), 64), 1.0, 1e-6);
}

TEST(SimilarityCalculatorTest, L2NormalizeRejectsZeroVector)
{
    std::vector<float> zeros(128, 0.0f);
    EXPECT_FALSE(SimilarityCalculator::l2NormalizeInPlace(zeros.data(), 128));
}
