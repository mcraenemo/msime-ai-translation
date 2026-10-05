#include "lattice_rerank.h"

#include "../neural/rescore_worker.h"

#include <numeric>
#include <optional>
#include <vector>

namespace quanpin
{

LatticeReranker make_neural_reranker(const neural::SentenceModel *model, const std::string &context,
                                     const neural::RerankOptions &options)
{
    if (model == nullptr)
    {
        return {};
    }
    // 在这里就把前文裁到模型真正会看的那几十个字：后台重排的结果表以「前文 + 候选句」为键，
    // 拿没裁过的整段历史当键的话，每上屏一个字键就变了，缓存永远打不中。
    const std::string trimmed = neural::last_characters(context, options.context_chars);
    return [model, context = trimmed, options](std::vector<LatticePath> &paths) -> bool {
        if (paths.size() < 2)
        {
            return false;
        }
        std::vector<std::string> sentences;
        std::vector<double> scores;
        sentences.reserve(paths.size());
        scores.reserve(paths.size());
        for (const LatticePath &path : paths)
        {
            sentences.push_back(path.sentence);
            scores.push_back(path.log_prob);
        }

        // 不在这里打分：交给后台线程，第一次必然落空，按词格的静态顺序先出候选。线程算完会叫
        // 醒会话重查一次，那一次才在结果表里拿到顺序。
        const std::optional<std::vector<std::size_t>> order =
            neural::RescoreWorker::instance().order_for(*model, context, sentences, scores, options);
        if (!order || order->size() != paths.size())
        {
            return false; // 还没算出来，或者模型自己弃权了：词格的顺序原样留着
        }

        std::vector<LatticePath> reordered;
        reordered.reserve(paths.size());
        for (std::size_t index : *order)
        {
            reordered.push_back(paths[index]);
        }
        paths = std::move(reordered);
        return true;
    };
}

LatticeReranker make_octagram_reranker(const std::shared_ptr<const gram::GramDb> &db, double weight)
{
    if (db == nullptr || !db->valid() || weight == 0.0)
    {
        return {};
    }
    return [db, weight](std::vector<LatticePath> &paths) -> bool {
        if (paths.size() < 2)
        {
            return false;
        }
        // 整句搭配分 = 相邻词对的搭配项之和（词对i的上下文取其前全部词的文本，
        // .gram 查询自己截尾窗），句尾词吃 rear 项。
        std::vector<double> scores(paths.size());
        for (size_t p = 0; p < paths.size(); ++p)
        {
            double bonus = 0;
            std::string context;
            for (size_t i = 0; i < paths[p].words.size(); ++i)
            {
                const bool is_rear = i + 1 == paths[p].words.size();
                bonus += db->query(context, paths[p].words[i], is_rear, gram::GrammarConfig{});
                context += paths[p].words[i];
            }
            scores[p] = paths[p].log_prob + weight * bonus;
        }
        std::vector<size_t> order(paths.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return scores[a] > scores[b]; });
        std::vector<LatticePath> reordered;
        reordered.reserve(paths.size());
        for (size_t index : order)
        {
            reordered.push_back(std::move(paths[index]));
        }
        paths = std::move(reordered);
        return true;
    };
}

} // namespace quanpin
