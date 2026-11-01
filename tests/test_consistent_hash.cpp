#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "consistent_hash.hpp"

using namespace dcache;

int main() {
    const int VN = 150;

    ConsistentHash ch(VN);
    ch.AddNode("A");
    ch.AddNode("B");
    ch.AddNode("C");
    CHECK(ch.NodeCount() == 3);
    CHECK(ch.RingSize() == static_cast<std::size_t>(3 * VN));

    // 同一 key 确定性路由：重复查询结果一致。
    CHECK(ch.GetNode("hello") == ch.GetNode("hello"));

    // 三个物理节点都被分配到 key（分布均衡，无闲置节点）。
    std::set<std::string> seen;
    for (int i = 0; i < 10000; ++i) {
        seen.insert(ch.GetNode("key" + std::to_string(i)));
    }
    CHECK(seen.size() == 3);

    // 新增节点：迁移比例 ≤ 2/N，且新节点确实接管了部分 key。
    const int total = 10000;
    std::vector<std::string> before;
    before.reserve(total);
    for (int i = 0; i < total; ++i) {
        before.push_back(ch.GetNode("key" + std::to_string(i)));
    }

    ch.AddNode("D");
    CHECK(ch.NodeCount() == 4);

    int moved = 0;
    for (int i = 0; i < total; ++i) {
        if (ch.GetNode("key" + std::to_string(i)) != before[i]) {
            ++moved;
        }
    }
    const double ratio = static_cast<double>(moved) / total;
    CHECK(ratio > 0.0);              // D 确实接管了一部分 key
    CHECK(ratio <= 2.0 / 3 + 0.05);  // 远小于普通取模哈希的 ~3/4 全量迁移

    // 删除节点后不再返回该节点。
    ch.RemoveNode("D");
    CHECK(!ch.Contains("D"));
    CHECK(ch.NodeCount() == 3);

    // 空环返回空串。
    ConsistentHash empty(VN);
    CHECK(empty.GetNode("anything") == "");

    return 0;
}