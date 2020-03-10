/*
 * Copyright (c) 2020 Peking University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Rock Lee
 */


#include <cstdint>

#include "mem/cache/prefetch_filter/base.hh"

class BaseCache;
struct BasePrefetchFilterParams;

namespace prefetch_filter {

BasePrefetchFilter* BasePrefetchFilter::onlyInstance_ = nullptr;
int64_t BasePrefetchFIlter::typeHash_ = -1;
uint64_t BasePrefetchFilter::cacheLineAddrMask_ = 0;
uint8_t BasePrefetchFilter::cacheLineOffsetBits_ = 0;

BasePrefetchFilter::BasePrefetchFilter(const BasePrefetchFilterParams *p) :
        statsPeriod_(p->stats_period),
        nextPeroidTick_(p->stats_period),
        enableStats_(p->enable_stats),
        enableFilter_(p->enable_filter),
        cacheLineAddrMask_(~(p->block_size - 1)) {
    int blockSize = p->block_size;
    cacheLineOffsetBits_ = 0;
    while(blockSize >> ++cacheLineOffsetBits_) {}
}

int BasePrefetchFilter::updateInstance(BasePrefetchFilter** ptr) {
    if (!onlyInstance_) {
        onlyInstance_ = this;
    } else {
        CHECK_RET_EXIT(this != onlyInstance_,
                "Update instance pointer more than once");
        *ptr = onlyInstance_;
        delete this;
    }
    return 0;
}

int BasePrefetchFilter::addCache(BaseCache* newCache) {
    int level = newCache->cacheLevel_;
    if (level >= caches_.size()) {
        caches_.resize(level + 1);
    }
    usefulTable_[newCache] = IdealPrefetchUsefulTable(newCache);
    caches_[level].push_back(newCache);
    return 0;
}

int BasePrefetchFilter::notifyCacheHit(BaseCache* cache,
        const PacketPtr& pkt, const uint64_t& hitAddr,
        const DataTypeInfo& info) {
    if (info.source = Dmd) {
        for (BaseCache* cache : pkt->caches_) {
            demandHit_[cache->cacheLevel_]++;
            for (const uint8_t cpuId : cache->cpuIds_) {
                (*demandHitCount_[cache->cacheLevel_])[cpuId]++;
                (*demandReqHitTotal_)[cpuId]++;
            }
        }
    }
    
    // 暂时不处理ICache的相关内容
    if (!cache->cacheLevel_) {
        return 0;
    }

    std::vector<Stats::Vector*> cacheStats =
            {l1PrefHitCount_[cache->cacheLevel_ - 1]};
    if (cache->cacheLevel_ == 3) {
        cacheStats.push_back(l2PrefHitCount_[1]);
    } else if (cache->cacheLevel == 2) {
        cacheStats.push_back(l2PrefHitCount_[0]);
    }
    
    // 依据源数据和目标数据类型进行统计数据更新
    if (info.source == Dmd) {
        if (info.target == Pref) {
            // Demand Request命中了预取数据
            CHECK_RET_EXIT(usefulTable_[cache].updateHit(pkt, hitAddr, Dmd,
                    cacheStats), "Failed to update when dmd hit pref data");
        }
    } else if (info.target == Pref) {
        // 预取命中了预取数据
        CHECK_RET_EXIT(usefulTable_[cache].updateHit(pkt, hitAddr, Pref,
                cacheStats), "Failed to update when useful pref hit %s",
                "pref data");
    }

    // 时间维度的检查与更新
    CHECK_RET_EXIT(checkUpdateTimingStats(),
            "Failed check & update timing stats information");
    return 0;
}

int BasePrefetchFilter::notifyCacheMiss(BaseCache* cache,
        const PacketPtr& pkt, const uint64_t& combinedAddr,
        const DataTypeInfo& info) {
    // 这里的info记录的source是新的Miss属性
    // 其中的target表示mshr中已存在的内容属性
    // prefetch->nulltype表示prefetch获得了一个新的mshr表项
    // 而非和一个demand类型的mshr合并，但是我们并不关心
    CHECK_ARGS_EXIT(info.source != Pref || info.target != Dmd,
            "Prefetch will not combined with demand in mshr.");
    if (info.source == Dmd) {
        for (BaseCache* cache : pkt->caches_) {
            demandMiss_[cache->cacheLevel_]++;
            for (const uint8_t cpuId : cache->cpuIds_) {
                (*demandReqMissCount_[cache->cacheLevel_])[cpuId]++;
                if (info.target == Pref && cache->cacheLevel_) {
                    (*shadowedPrefCount_[cache->cacheLevel_ - 1])[cpuId]++;
                }
            }
        }
        if (info.target != Dmd) {
            // 预取命中了预取数据
            CHECK_RET_EXIT(usefulTable_[cache].updateMiss(pkt),
                    "Failed to update when demand request miss");
        }
    } else if (info.target == NullType && pkt->caches_.size() == 1 &&
            pkt->caches_.front() == cache) {
        // 一个新生成的没有合并的预取，需要添加对应信息进行追踪
        CHECK_RET_EXIT(usefulTable_[cache].newPref(pkt),
                "Failed to add new prefetch info to useful table");
    }
    
    // 时间维度的检查与更新
    CHECK_RET_EXIT(checkUpdateTimingStats(),
            "Failed check & update timing stats information");
    return 0;
}

int BasePrefetchFilter::notifyCacheFill(BaseCache* cache,
        const PacketPtr &pkt, const uint64_t& evictedAddr,
        const DataTypeInfo& info) {
    if (!cache->cacheLevel_) {
        return 0;
    }
    
    if (info.source == Dmd && info.target == Pref) {
        // 如果一个Demand Request替换了一个预取请求的数据
        // 则会将对应预取从当前Cache记录表中剔除
        CHECK_RET_EXIT(usefulTable_[cache].updateEvict(evictedAddr),
                "Failed to remove pref from the table");
    } else if (info.source == Pref) {
        // 预取替换了一个Demand Request请求的数据
        // 就需要对相关的替换信息进行记录
        if (info.target == Dmd) {
            CHECK_RET_EXIT(usefulTable_[cache].addPref(pkt, evictedAddr),
                    "Failed to add pref replacing dmd in the table");
        } else {
            int result;
            CHECK_RET_EXIT(result = usefulTable_[cache].isPrefHit(pkt->addr),
                    "Pref not exist in the table");
            if (result) {
                // 如果一个预取被Demand Request覆盖，那么它将会被看作是
                // 一个Demand Request，成为一个有害情况的备选项
                CHECK_RET_EXIT(usefulTable_[cache].addPref(pkt,
                        evictedAddr), "Failed to add pref replacing %s",
                        "used pref in the table");
            } else {
                // 如果一个预取替换了一个没有被Demand Request命中过的无用预取
                // 则原预取将会被剔除，并进行记录，同时新的预取替换对象会继承
                // 之前替换对象的地址
                CHECK_RET_EXIT(usefulTable_[cache].replaceEvict(pkt,
                        evictedAddr),
                        "Failed to replace old pref with new pref");
            }
        }
    }
    
    // 时间维度的检查与更新
    CHECK_RET_EXIT(checkUpdateTimingStats(),
            "Failed check & update timing stats information");
    return 0;
}

int BasePrefetchFilter::filterPrefetch(BaseCache* cache,
        const PacketPtr &pkt, const PrefetchInfo& info) {
    // 默认的过滤器没有实现，因此不会改变预取数据的存放位置
    return cache->cacheLevel_;
}

int BasePrefetchFilter::genHash(const std::string& name) {   
    register int64_t hash = 0;
    int index = 0;
    while(index < name.length()) {
        hash = hash * 131 + name[index++];
    }
    hash = abs(hash);
    if (typeHash_ = -1) {
        typeHash_ = hash;
    } else {
        CHECK_RET(typeHash_ == hash,
                "Trying to initiate different types of prefetch filter");
    }
    return 0;
}

void BasePrefetchFilter::init() {   
    maxCacheLevel_ = caches_.size() - 1;
    numCpus_ = caches_[0].size();
    for (int i = 0; i <= maxCacheLevel_; i++) {
        usePref_[level] = caches_[i][0]->prefetcher != NULL;
    }
}

int BasePrefetchFilter::checkUpdateTimingStats() {
    if (enableStats_ && curTick() > nextPeriodTick_) {
        timingStatsPeriodCount_++;

        // 更新时间维度的统计数据
        for (auto mapPair : usefulTable_) {
            CHECK_RET_EXIT(mapPair->second.updatePrefTiming(
                    prefTotalUsefulValue_, prefUsefulDegree_, prefUsefulType_,
                    prefDegreeCount_), "Failed to update stats timingly");
        }
        
        // 打印时间维度的信息
        std::stringstream outputInfo;
        outputInfo << "Stats Period Id: " << timingStatsPeriodCount_ <<
                std::endl;
        outputInfo << "\tDemand Hit Count: " << demandHit_[0] << ", " <<
                demandHit_[1] << ", " << demandHit_[2] << ", " <<
                demandHit_[3] << ".\n";
        outputInfo << "\tDemand Hit Count: " << demandMiss_[0] << ", " <<
                demandMiss_[1] << ", " << demandMiss_[2] << ", " <<
                demandMiss_[3] << ".\n";
        outputInfo << "\tPref Type Count: \n";
        for (int i = 0; i < 3; i++) {
            outputInfo << "\t\t" << BaseCache::levelName[i + 1] << "(";
            for (int j = 0; j < 2; j++) {
                std::string usefulTypeStr = j ? "cross_core" : "single_core";
                outputInfo << usefulTypeStr << "): ";
                for (int k = 0; k < 5; k++) {
                    outputInfo << prefDegreeCount_[i][j][k] << ", ";
                }
            }
            outputInfo << std::endl;
        }
        DPRINTF(PrefetchFilter, "%s", outputInfo.str());

        // 方案1：该方案在全局固定的分割间隔内进行采样，采样范围是对齐的
        /*
        while (nextPeriodTick_ - curTick() < statsPeriod_) {
            nextPeriodTick_ += statsPeriod_;
        }
        */
        // 方案2：动态的采样，每次都从当前的位置开启，采样范围不对齐
        nextPeriodTick_ = curTick() + statsPeriod_;
        
        // 重置时间维度的统计数据
        memset(demandHit_, 0, sizeof(demandHit_);
        memset(demandMiss_, 0, sizeof(demandMiss_);
        memset(prefDegreeCount_, 0, sizeof(prefDegreeCount_);
    }
    // 重置空统计变量防止溢出
    emptyStatsVar = 0;
    return 0;
}

void BasePrefetchFilter::regStats() {
    ClockedObject::regStats();

    int i, j, k;
    
    // 进行初始化操作
    init();

    emptyStatsVar_
            .name(name() + ".empty_stats")
            .desc("Empty stats variable for deleted stats")
            .flag(nozero);
    for (j = 0; j < numCpus_; j++) {
        emptyStatsVar_[i]->subname(j, std::string("cpu") +
                std::to_string(j));
    }

    if (enableStats_) {
        demandReqHitTotal_ = new Stats::Vector();
                .name(name() + ".total_demand_requests_hit")
                .desc("Number of demand requests")
                .flag(total);
        for (j = 0; j < numCpus_; j++) {
            demandReqHitTotal_[i]->subname(j, std::string("cpu") +
                    std::to_string(j));
        }
    }

    for (i = 0; i < 4; i++) {
        if (i <= maxCacheLevel_ && enableStats_) {
            demandReqHitCount_[i] = new Stats::Vector();
            demandReqHitCount_[i]
                    ->name(name() + ".demand_requests_hit_" +
                            BaseCache::levelName_[i])
                    .desc(std::string("Number of demand requests hit in ") +
                            BaseCache::levelName_[i])
                    .flag(total);
            for (j = 0; j < numCpus_; j++) {
                demandReqHitCount_[i]->subname(j, std::string("cpu") +
                        std::to_string(j));
            }
        } else {
            demandReqHitCount_[i] = &emptyStatsVar_;
        }
    }
    
    for (i = 0; i < 4; i++) {
        if (i <= maxCacheLevel_ && enableStats_) {
            demandReqMissCount_[i] = new Stats::Vector();
            demandReqMissCount_[i]
                    ->name(name() + ".demand_requests_miss_" +
                            BaseCache::levelName_[i])
                    .desc(std::string("Number of demand requests miss in ") +
                            BaseCache::levelName_[i])
                    .flag(total);
            for (j = 0; j < numCpus_; j++) {
                demandReqMissCount_[i]->subname(j, std::string("cpu") +
                        std::to_string(j));
            }
        } else {
            demandReqMissCount_[i] = &emptyStatsVar_;
        }
    }
    
    if (usePref_[1] && enableStats_) {
        for (i = 0; i < 3; i++) {
            l1PrefHitCount_[i] = new Stats::Vector();
            l1PrefHitCount_[i]
                    ->name(name() + ".L1_prefetch_requests_hit_" +
                            BaseCache::levelName_[i + 2])
                    .desc(std::string("Number of prefetches request from L1 " +
                            "hit in " + BaseCache::levelName_[i + 1])
                    .flag(total);
            for (j = 0; j < numCpus_; j++) {
                l1PrefHitCount_[i]->subname(j, std::string("cpu") +
                        std::to_string(j));
            }
        }
    } else {
        for (i = 0; i < 3; i++) {
            l1PrefHitCount_[i] = &emptyStatsVar_;
        }
    }

    if (usePref_[2] && enableStats_) {
        for (i = 0; i < 2; i++) {
            l2PrefHitCount_[i] = new Stats::Vector();
            l2PrefHitCount_[i]
                    ->name(name() + ".L2_prefetch_requests_hit_" +
                            BaseCache::levelName_[i + 3])
                    .desc(std::string("Number of prefetches request from") +
                            " L2 hit in " + BaseCache::levelName_[i + 3])
                    .flag(total);
            for (j = 0; j < numCpus_; j++) {
                l2PrefHitCount_[i]->subname(j, std::string("cpu") +
                        std::to_string(j));
            }
        }
    } else {
        for (i = 0; i < 2; i++) {
            l2PrefHitCount_[i] = &emptyStatsVar_;
        }
    }

    for (i = 0; i < 3; i++) {
        if (usePref_[i + 1] && enableStats_) {
            shadowedPrefCount_[i] = new Stats::Vector();
            shadowedPrefCount_[i]
                    ->name(name() + "." + BaseCache::levelName_[i + 1] +
                            "_shadowed_prefetches")
                    .desc(std::string("Number of prefetches shadowed by demand "
                            "requests from " + BaseCache::levelName_[i + 1])
                    .flag(total);
            for (j = 0; j < numCpus_; j++) {
                shadowedPrefHitCount_[i]->subname(j, std::string("cpu") +
                        std::to_string(j));
            }
        } else {
            shadowedPrefCount_[i] = &emptyStatsVar_;
        }
    }

    for (i = 0; i < 3; i++) {
        if (usePref_[i + 1] && enableStats_) {
            std::string usefulTypeStr;
            for (j = 0; j < 2; j++) {
                usefulTypeStr = j ? "cross_core" : "single_core";
                prefTotalUsefulValue_[i][j] = new Stats::Vector();
                prefTotalUsefulValue_[i][j]
                        ->name(name() + "." + BaseCache::levelName_[i + 1] +
                                "_prefetch_" + usefulTypeStr + "_useful_value")
                        .desc(std::string("Total") + usefulTypeStr +
                                "useful value for of prefetches from" +
                                BaseCache::levelName_[i + 1])
                        .flag(total);
                for (k = 0; k < numCpus_; k++) {
                    shadowedPrefHitCount_[i][j]->subname(k, std::string("cpu") +
                            std::to_string(k));
               }
            }
        } else {
            for (j = 0; j < 2; j++) {
                prefTotalUsefulValue_[i][j] = &emptyStatsVar_;
            }
        }
    }

    for (i = 0; i < 3; i++) {
        if (usePref_[i + 1] && enableStats_) {
            std::string usefulTypeStr;
            for (j = 0; j < 2; j++) {
                usefulTypeStr = j ? "cross_core" : "single_core";
                for (k = 0; k < 5; k++) {
                    std::string degreeStr = std::to_string(k);
                    prefUsefulDegree_[i][j][k] = new Stats::Vector();
                    prefUsefulDegree_[i][j][k]
                            ->name(name() + "." +
                                    BaseCache::levelName_[i + 1] +
                                    "_prefetch_" + usefulTypeStr +
                                    "_useful_degree_" + degreeStr)
                            .desc(std::string("Number of prefetches with " +
                                    usefulType + " useful degree " +
                                    degreeStr + " from " +
                                    BaseCache::levelName_[i + 1])
                        .flag(total);
                for (int n = 0; n < numCpus_; n++) {
                    prefUsefulDegreei_[i][j][k]->subname(n, std::string("cpu") +
                            std::to_string(n));
                }
            }
        } else {
            for (j = 0; j < 2; j++) {
                for (k = 0; k < 5; k++) {
                    prefUsefulDegree_[i][j][k] = &emptyStatsVar_;
                }
            }
        }
    }
    
    for (PrefUsefulType& type : PrefUsefulTypeList) {
        for (i = 0; i < 3; i++) {
            if (usePref_[i + 1] && enableStats_) {
                prefUsefulType_[i].push_back(new Stats::Vector());
                prefUsefulType_[i].back()
                        ->name(name() + "." + BaseCache::levelName_[i + 1] +
                                + "_" + type.name + "_prefetches")
                        .desc(std::string("Number of ") + type.name +
                                " prefetches from" +
                                BaseCache::levelName_[i + 1])
                        .flag(total);
                for (j = 0; j < numCpus_; j++) {
                    prefUsefulType_[i].back()->subname(j, std::string("cpu") +
                            std::to_string(j));
                }
            } else {
                prefUsefulType_[i].push_back(&emptyStatsVar_);
            }
        }
    }
}

} // namespace prefetch_filter
