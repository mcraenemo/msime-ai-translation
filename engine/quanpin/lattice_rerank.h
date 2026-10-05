#pragma once

// Bridges the word lattice's n-best paths to the neural sentence model.
//
// word_lattice.* knows nothing about the model and engine/neural knows nothing about lattices; this
// is the one place that speaks both. Quanpin and shuangpin share it because their sentence pipelines
// are the same lattice over the same syllables.

#include "../neural/neural_decoder.h"
#include "../ngram/octagram/octagram_gram.h"
#include "word_lattice.h"

#include <string>

namespace quanpin
{

// A reranker for merge_lattice_candidates that reorders paths by the neural model, or an empty
// std::function when `model` is null (the model file is absent, or the feature is switched off).
// An empty reranker is not an error: the lattice's own order stands.
//
// `context` is the text already committed before the composition, used as the model's conditioning
// prefix. Empty is fine and simply scores each sentence on its own. Pass the whole history: it is
// trimmed here to the last `options.context_chars` characters, which is what actually gets scored.
LatticeReranker make_neural_reranker(const neural::SentenceModel *model, const std::string &context,
                                     const neural::RerankOptions &options = {});

// A reranker that reorders the lattice's n-best by the octagram grammar model: each path keeps its
// trigram log_prob and gains weight * (sum of char-level collocation terms over adjacent word pairs,
// with the rear term on the sentence-final word). Empty std::function when db is null or weight is
// zero — the feature is off, the lattice's own order stands. Synchronous and read-only (mmap'd
// trie), unlike the neural reranker's background thread.
LatticeReranker make_octagram_reranker(const std::shared_ptr<const gram::GramDb> &db, double weight);

} // namespace quanpin
