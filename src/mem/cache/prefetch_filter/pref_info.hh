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

#ifndef __MEM_CACHE_PREFETCH_FILTER_PREF_INFO_HH__
#define __MEM_CACHE_PREFETCH_FILTER_PREF_INFO_HH__

#include <cstdint>
#include <string>
#include <vector>
#include <list>
#include <map>

#include "base/statistics.hh"
class BaseCache;
class Packet;

namespace prefetch_filter {

typedef Packet *PacketPtr;

// 该变量用于进行预取校正（防止特殊的预取不能得到释放）
extern Tick maxResponseGap_;

// 用于进行时间计时，方便确认bug位置
extern Tick timerPrintGap_;

// 当前系统下的CPU个数，由BasePrefetchFilter初始化
extern int numCpus_;

// 该变量表示一个无效地址
extern uint64_t invalidBlkAddr_;

// 依据一组Cache指针生成对应的CPUID位向量
uint64_t generateCoreIDMap(const std::set<BaseCache*>& caches);

// 依据预取的信息生成当前预取的全局唯一ID
uint64_t generatePrefIndex(const PacketPtr pkt);

// 数据类型
enum DataType {NullType, Dmd, Pref, PendingPref};

// 获取一个数据类型的字符串表示
std::string getDataTypeString(const DataType type);

// Miss以及Fill时候的信息
struct DataTypeInfo {
    // 插入的数据是什么属性
    DataType source;
    // 被替换的数据是什么属性
    DataType target;
};

// 一个预取信息项的索引和有效位数信息
class IndexInfo {
public:
    // 默认构造函数
    IndexInfo () {}

    // 构造函数
    IndexInfo(const uint8_t index, const uint8_t bits,
            const std::string& varName) :
            index_(index), bits_(bits), varName_(varName) {}

    // 信息项索引
    uint8_t index_ = 0;

    // 信息项有效位数
    uint8_t bits_ = 0;
    
    // 信息项的变量名称
    std::string varName_;
};

// 记录字符串到信息项映射的数据
extern std::map<std::string, IndexInfo> PrefInfoIndexMap;

// 该变量只是用来避免unsed-variable提示
extern std::vector<int> PrefInfoIndexes;

// 注册一个新的信息项相关的函数
int addNewInfo(const std::string& name, const std::string& varName,
        const uint8_t bits);

// 注册新信息项的宏
#define DEF_INFO(VARNAME, STRING, BITS) \
    const int VARNAME = addNewInfo(#STRING, #VARNAME, BITS);

// 预取信息类，用来从预取器传递预取信息到PPFE中
class PrefetchInfo {
private:
    // 信息项主体
    std::vector<uint32_t> info_;

    // 判定某一个信息是否有效，最多支持32各信息
    uint64_t valid_ = 0;

public:
    // 写入一个新的信息
    int setInfo(const uint8_t index, const uint32_t value);
    
    // 写入一个新的信息
    int setInfo(const std::string& name, const uint32_t value);
    
    // 读取一个新的信息，读取成功返回1，失败则返回0，错误则返回-1
    int getInfo(const uint8_t index, uint32_t* value) const;
    
    // 读取一个新的信息，读取成功返回1，失败则返回0，错误则返回-1
    int getInfo(const std::string& name, uint32_t* value) const;
};

// 预取分类的结构体
class PrefUsefulType {
public:
    PrefUsefulType(const int index, const std::string& name,
            std::function<bool(const uint64_t&, const uint64_t&,
            const uint64_t&,const uint64_t&)> judgeFunc) :
            index_(index), name_(name), isType(judgeFunc) {}

public:
    // 预取类型对应的Index
    const int index_;

    // 预取分类的名称
    const std::string name_;

    // 进行判断的函数
    std::function<bool(const uint64_t&, const uint64_t&, const uint64_t&,
            const uint64_t&)> isType;
};

// 记录每一个预取分类的名称
extern std::vector<PrefUsefulType> PrefUsefulTypeList;

// 注册一个新的信息项相关的函数
int addNewPrefUsefulType(const std::string& name,
        std::function<bool(const uint64_t&, const uint64_t&, const uint64_t&,
        const uint64_t&)> judgeFunc);

#define TOTAL_DEGREE 5
#define PREF_DEGREE_HARM -4
#define PREF_DEGREE_USELESS 0
#define PREF_DEGREE_USEFUL 4

class PrefetchUsefulInfo {
public:
    // 默认初始化函数
    PrefetchUsefulInfo() {}
    
    // 依据CPU的个数进行大小配置
    PrefetchUsefulInfo(BaseCache* srcCache, const uint64_t& index,
            const uint64_t& addr);
    
    // 更新一个预取有效命中，同时对命中统计数据进行更新
    int updateUse(const std::set<uint8_t>& cpuIds);

    // 更新一个预取有害命中
    int updateHarm(const std::set<uint8_t>& cpuIds);

    // 添加当前预取在某一个Cache中的替换地址
    int addReplacedAddr(BaseCache* cache, const uint64_t& replacedAddr);

    // 重新设置当前预取在某一个Cache中的替换地址
    int resetReplacedAddr(BaseCache* cache, const uint64_t& replacedAddr);

    // 删除一个预取在某一个Cache中的替换地址，并返回可能相关的需要无效化Cache
    int rmReplacedAddr(BaseCache* cache,
            std::set<BaseCache*>* correlatedCaches);

    // 获取当前预取在某一个Cache中的替换地址
    int getReplacedAddr(BaseCache* cache, uint64_t* replacedAddr);

    // 获取预取给定Cache相连接的下层Cache（都包含该预取）
    int getCorrelatedCaches(BaseCache* cache,
            std::set<BaseCache*>* correlatedCaches);

    // 获取当前Cache所有的所在Cache记录
    int getLocatedCaches(std::set<BaseCache*>* caches);

    // 判断当前的预取是不是一个足够有用的预取
    int isUseful();
  
    // 判断当前的预取是否可以被删除
    int canDelete();

    // 判断当前的预取是那一类预取
    int getTypeIndex();

private:
    // 获取两组cpuid对应的但和有效性和多核有效性信息
    int getUpdateValue(const std::set<uint8_t>& srcCpuIds,
            const std::set<uint8_t>& targetCpuIds,
            int* singleCoreUpdate, int* crossCoreUpdate);

public:
    // 存放统计数据的结构体
    // 没有记录Cycle是因为实际上每一个有用或者有害带来的
    // 时钟周期损失/节省均为LLC的一次Miss Latency
    struct Info {
        // 单核心预取产生的有效命中次数
        int singleCoreUsefulCount_ = 0;
        
        // 单核心预取有害统计的次数
        int singleCoreHarmCount_ = 0;
        
        // 多核心预取产生的有效命中次数
        int crossCoreUsefulCount_ = 0;
        
        // 多核心预取有害统计的次数
        int crossCoreHarmCount_ = 0;
    } info_;
    
    // 发射当前预取的Cache等级
    BaseCache* srcCache_ = nullptr;

    // 该信息对应的哈希索引
    const uint64_t index_ = 0;

    // 对应预取的地址
    const uint64_t addr_ = invalidBlkAddr_;

    // 该预取是不是一个降级预取（用于预取校正）
    bool isLevelDown_ = false;

    // 该预取的注册时间（用于预取校正）
    Tick regTime_ = 0;

private:
    // 当前预取对应的替换数据地址，考虑到预取的贯穿效应，
    // 需要记录多个被替换的地址信息
    std::map<BaseCache*, uint64_t> replacedAddress_;
};

} // namespace prefetch_filter

#endif // __MEM_CACHE_PREFETCH_FILTER_PREF_INFO_HH__
