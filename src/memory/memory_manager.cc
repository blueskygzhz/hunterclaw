#include "hunterclaw/memory/memory.h"
#include "hunterclaw/memory/embedding_provider.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace hunterclaw {
namespace memory {

// 相似度计算实现
namespace similarity {

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        throw std::invalid_argument("Vector dimensions must match");
    }
    
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    
    if (norm_a == 0.0 || norm_b == 0.0) {
        return 0.0;
    }
    
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

double euclidean(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        throw std::invalid_argument("Vector dimensions must match");
    }
    
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double diff = a[i] - b[i];
        sum += diff * diff;
    }
    
    return std::sqrt(sum);
}

double dot_product(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        throw std::invalid_argument("Vector dimensions must match");
    }
    
    double result = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        result += a[i] * b[i];
    }
    
    return result;
}

} // namespace similarity

// MemoryManager实现
std::shared_ptr<MemoryManager> MemoryManager::create(
    std::shared_ptr<MemoryStorage> storage,
    std::shared_ptr<EmbeddingProvider> embedding,
    const SearchConfig& config
) {
    auto manager = std::shared_ptr<MemoryManager>(new MemoryManager());
    manager->storage_ = storage;
    manager->embedding_ = embedding;
    manager->config_ = config;
    return manager;
}

std::string MemoryManager::add_memory(
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    // 生成embedding
    auto embedding = embedding_->embed_query(content);
    
    // 存储到数据库
    return storage_->add(content, embedding, metadata);
}

std::vector<SearchResult> MemoryManager::search(const std::string& query) {
    // 1. 生成查询embedding
    auto query_embedding = embedding_->embed_query(query);
    
    // 2. 向量搜索
    int vector_limit = config_.max_results * config_.hybrid.candidate_multiplier;
    auto vector_results = storage_->vector_search(
        query_embedding,
        vector_limit,
        config_.min_score
    );
    
    // 3. 如果不启用混合搜索,直接返回向量结果
    if (!config_.hybrid.enabled) {
        return std::vector<SearchResult>(
            vector_results.begin(),
            vector_results.begin() + std::min(config_.max_results, static_cast<int>(vector_results.size()))
        );
    }
    
    // 4. 全文搜索 (BM25)
    auto text_results = storage_->text_search(query, vector_limit);
    
    // 5. 混合排名
    auto combined = hybrid_rank(vector_results, text_results);
    
    // 6. MMR重排 (增加多样性)
    if (config_.hybrid.mmr.enabled) {
        combined = maximal_marginal_relevance(combined, query_embedding);
    }
    
    // 7. 时间衰减
    if (config_.hybrid.temporal_decay.enabled) {
        combined = apply_temporal_decay(combined);
        
        // 重新排序
        std::sort(combined.begin(), combined.end(),
                 [](const SearchResult& a, const SearchResult& b) {
                     return a.score > b.score;
                 });
    }
    
    // 8. 截断到max_results
    if (combined.size() > static_cast<size_t>(config_.max_results)) {
        combined.resize(config_.max_results);
    }
    
    return combined;
}

bool MemoryManager::remove_memory(const std::string& id) {
    return storage_->remove(id);
}

void MemoryManager::update_config(const SearchConfig& config) {
    config_ = config;
}

std::vector<SearchResult> MemoryManager::hybrid_rank(
    const std::vector<SearchResult>& vector_results,
    const std::vector<SearchResult>& text_results
) {
    std::map<std::string, double> scores;
    std::map<std::string, SearchResult> results_map;
    
    // 向量搜索得分 (已经是余弦相似度 0-1)
    for (const auto& result : vector_results) {
        scores[result.id] = result.score * config_.hybrid.vector_weight;
        results_map[result.id] = result;
    }
    
    // 文本搜索得分 (BM25需要归一化)
    if (!text_results.empty()) {
        double max_bm25 = text_results[0].score;
        
        for (const auto& result : text_results) {
            double normalized_score = (max_bm25 > 0) ? (result.score / max_bm25) : 0.0;
            
            if (scores.find(result.id) != scores.end()) {
                scores[result.id] += normalized_score * config_.hybrid.text_weight;
            } else {
                scores[result.id] = normalized_score * config_.hybrid.text_weight;
                results_map[result.id] = result;
            }
        }
    }
    
    // 转换为vector并排序
    std::vector<SearchResult> combined;
    for (const auto& pair : scores) {
        auto result = results_map[pair.first];
        result.score = pair.second;
        combined.push_back(result);
    }
    
    std::sort(combined.begin(), combined.end(),
             [](const SearchResult& a, const SearchResult& b) {
                 return a.score > b.score;
             });
    
    return combined;
}

std::vector<SearchResult> MemoryManager::maximal_marginal_relevance(
    const std::vector<SearchResult>& candidates,
    const std::vector<float>& query_embedding
) {
    if (candidates.empty()) {
        return {};
    }
    
    std::vector<SearchResult> selected;
    std::vector<SearchResult> remaining = candidates;
    
    double lambda = config_.hybrid.mmr.lambda;
    int k = config_.max_results;
    
    while (selected.size() < static_cast<size_t>(k) && !remaining.empty()) {
        int best_index = -1;
        double best_mmr_score = -1.0;
        
        for (size_t i = 0; i < remaining.size(); ++i) {
            const auto& candidate = remaining[i];
            
            // Skip if no embedding
            if (!candidate.embedding.has_value()) {
                continue;
            }
            
            // 相关性得分 (与查询的相似度)
            double relevance = similarity::cosine(query_embedding, candidate.embedding.value());
            
            // 多样性惩罚 (与已选结果的最大相似度)
            double max_similarity = 0.0;
            for (const auto& selected_result : selected) {
                if (selected_result.embedding.has_value()) {
                    double sim = similarity::cosine(candidate.embedding.value(), selected_result.embedding.value());
                    max_similarity = std::max(max_similarity, sim);
                }
            }
            
            // MMR得分: λ * 相关性 - (1-λ) * 多样性惩罚
            double mmr_score = lambda * relevance - (1.0 - lambda) * max_similarity;
            
            if (mmr_score > best_mmr_score) {
                best_mmr_score = mmr_score;
                best_index = static_cast<int>(i);
            }
        }
        
        if (best_index >= 0) {
            auto result = remaining[best_index];
            result.score = best_mmr_score;
            selected.push_back(result);
            remaining.erase(remaining.begin() + best_index);
        } else {
            break;
        }
    }
    
    return selected;
}

std::vector<SearchResult> MemoryManager::apply_temporal_decay(
    const std::vector<SearchResult>& results
) {
    auto now = std::chrono::system_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    double half_life_ms = config_.hybrid.temporal_decay.half_life_days * 24 * 60 * 60 * 1000;
    
    std::vector<SearchResult> decayed;
    for (const auto& result : results) {
        SearchResult decayed_result = result;
        
        // 计算时间差
        int64_t age_ms = now_ms - result.timestamp;
        
        // 指数衰减: score * 0.5^(age / half_life)
        double decay_factor = std::pow(0.5, age_ms / half_life_ms);
        decayed_result.score *= decay_factor;
        
        decayed.push_back(decayed_result);
    }
    
    return decayed;
}

} // namespace memory
} // namespace hunterclaw
