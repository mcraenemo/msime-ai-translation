#pragma once

#include <string>

// 整句联想的开关。全部默认关闭，由用户在设置里手动开启。
//
// 整句候选最多四行：词格首选、Google 解码器，以及两个神经模型的首选各一条。
//
// 前两个各是一条独立来源的整句。
//
// 后两个不一样。神经模型不再自己造句，它只在词格内部解出的最多 12 条 n-best 里挑一条（见
// engine/neural/neural_decoder.h）。它们不依赖 word_lattice 的显示开关：只开神经时词格仍会在内部
// 生成备选，但只显示神经选中的句子；同时开 word_lattice 时才额外保留词格首选作对照。两个神经
// 开关都开时各自独立打分，各出一条首选；两边选中相同文本时合并成一条。
struct SentenceAssociationOptions
{
    bool word_lattice = false;           // kenlm 词格整句，CandidateSource::Generated
    bool google = false;                 // Google 解码器整句，CandidateSource::Fallback
    bool neural_desktop = false;         // 用高精度档的神经模型重排词格整句
    bool neural_keyboard = false;        // 用快速档的神经模型重排词格整句
    bool show_next_on_duplicate = false; // 某来源首选被去重时，显示该来源下一条不同结果

    // 万象语法模型（octagram .gram）。宿主按模型包解析出 .gram 的路径传入，空 = 关闭；
    // 路径无效时词格静默退回无搭配打分，与其他整句来源的降级范式一致。
    // additive 在词格解码里逐边叠加字级搭配分（weight 缩放到 log10 域）；
    // rerank 在解出的 n-best 上按整句搭配分重排，以独立来源行（Collocation）呈现。
    // 两者可同时开：一个影响解码、一个影响呈现，各自的权重分开调。
    std::string collocation_model;           // .gram 文件路径（UTF-8）
    double collocation_weight = 0.1;         // additive 的线性权重
    bool collocation_rerank = false;         // n-best 重排开关
    double collocation_rerank_weight = 0.05; // 重排的线性权重

    bool operator==(const SentenceAssociationOptions &other) const
    {
        return word_lattice == other.word_lattice && google == other.google && neural_desktop == other.neural_desktop &&
               neural_keyboard == other.neural_keyboard && show_next_on_duplicate == other.show_next_on_duplicate &&
               collocation_model == other.collocation_model && collocation_weight == other.collocation_weight &&
               collocation_rerank == other.collocation_rerank &&
               collocation_rerank_weight == other.collocation_rerank_weight;
    }
    bool operator!=(const SentenceAssociationOptions &other) const
    {
        return !(*this == other);
    }
};
