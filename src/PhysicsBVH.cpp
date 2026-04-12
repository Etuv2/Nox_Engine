#include "PhysicsBVH.h"
#include "RigidBody.h"
#include <algorithm>
#include <stack>
#include <cmath>

PhysicsBVH::PhysicsBVH() {
    m_nodes.reserve(256);
    m_proxies.reserve(128);
}

PhysicsBVH::ProxyID PhysicsBVH::CreateProxy(const std::shared_ptr<RigidBody>& body, float margin) {
    if (!body) return NULL_PROXY;
    
    // Create proxy
    ProxyID proxyId = m_nextProxyId++;
    
    Proxy proxy;
    proxy.bodyId = body->GetBodyID();
    proxy.body = body;
    proxy.isStatic = body->IsStatic();
    proxy.aabbMin = body->getAABBMin();
    proxy.aabbMax = body->getAABBMax();
    proxy.fatAABBMin = proxy.aabbMin - glm::vec3(margin);
    proxy.fatAABBMax = proxy.aabbMax + glm::vec3(margin);
    
    // Store proxy
    if (proxyId >= static_cast<ProxyID>(m_proxies.size())) {
        m_proxies.resize(proxyId + 1);
    }
    m_proxies[proxyId] = proxy;
    m_proxyCount++;
    
    // Create leaf node
    int32_t nodeId = AllocateNode();
    Node& node = m_nodes[nodeId];
    node.aabbMin = proxy.fatAABBMin;
    node.aabbMax = proxy.fatAABBMax;
    node.proxyIndex = proxyId;
    node.height = 0;
    
    m_proxyToNode[proxyId] = nodeId;
    
    // Insert into tree
    InsertLeaf(nodeId);
    
    return proxyId;
}

void PhysicsBVH::DestroyProxy(ProxyID proxyId) {
    auto it = m_proxyToNode.find(proxyId);
    if (it == m_proxyToNode.end()) return;
    
    int32_t nodeId = it->second;
    
    RemoveLeaf(nodeId);
    FreeNode(nodeId);
    
    m_proxyToNode.erase(it);
    m_proxyCount--;
}

bool PhysicsBVH::UpdateProxy(ProxyID proxyId, const glm::vec3& newMin, const glm::vec3& newMax, float margin) {
    auto it = m_proxyToNode.find(proxyId);
    if (it == m_proxyToNode.end()) return false;
    
    Proxy& proxy = m_proxies[proxyId];
    proxy.aabbMin = newMin;
    proxy.aabbMax = newMax;
    
    // Check if still inside fat AABB
    if (newMin.x >= proxy.fatAABBMin.x && newMin.y >= proxy.fatAABBMin.y && newMin.z >= proxy.fatAABBMin.z &&
        newMax.x <= proxy.fatAABBMax.x && newMax.y <= proxy.fatAABBMax.y && newMax.z <= proxy.fatAABBMax.z) {
        return false; // No update needed
    }
    
    // Need to update - compute new fat AABB
    proxy.fatAABBMin = newMin - glm::vec3(margin);
    proxy.fatAABBMax = newMax + glm::vec3(margin);
    
    int32_t nodeId = it->second;
    
    // Remove and reinsert
    RemoveLeaf(nodeId);
    
    Node& node = m_nodes[nodeId];
    node.aabbMin = proxy.fatAABBMin;
    node.aabbMax = proxy.fatAABBMax;
    
    InsertLeaf(nodeId);
    
    return true;
}

void PhysicsBVH::QueryPairs(std::vector<Pair>& pairs) const {
    pairs.clear();
    
    if (m_root == NULL_PROXY) return;

    std::vector<ProxyID> proxyIds;
    proxyIds.reserve(m_proxyToNode.size());
    for (const auto& [proxyId, nodeId] : m_proxyToNode) {
        (void)nodeId;
        proxyIds.push_back(proxyId);
    }
    std::sort(proxyIds.begin(), proxyIds.end());
    
    // For each proxy, query against all others
    for (ProxyID proxyId : proxyIds) {
        const Proxy& proxy = m_proxies[proxyId];
        
        // Query tree for overlapping proxies
        std::stack<int32_t> stack;
        stack.push(m_root);
        
        while (!stack.empty()) {
            int32_t currentId = stack.top();
            stack.pop();
            
            if (currentId == NULL_PROXY) continue;
            
            const Node& node = m_nodes[currentId];
            
            if (!Overlaps(proxy.fatAABBMin, proxy.fatAABBMax, node.aabbMin, node.aabbMax)) {
                continue;
            }
            
            if (node.IsLeaf()) {
                ProxyID otherProxyId = node.proxyIndex;
                if (otherProxyId > proxyId) { // Avoid duplicates
                    const Proxy& otherProxy = m_proxies[otherProxyId];
                    
                    // Skip static-static pairs
                    if (proxy.isStatic && otherProxy.isStatic) continue;
                    
                    // Check layer/mask filtering via bodies
                    auto bodyA = proxy.body.lock();
                    auto bodyB = otherProxy.body.lock();
                    if (bodyA && bodyB && bodyA->canCollideWith(*bodyB)) {
                        pairs.push_back({proxyId, otherProxyId});
                    }
                }
            } else {
                stack.push(node.left);
                stack.push(node.right);
            }
        }
    }
}

void PhysicsBVH::Query(const glm::vec3& min, const glm::vec3& max,
                       std::function<bool(ProxyID)> callback) const {
    if (m_root == NULL_PROXY) return;
    
    std::stack<int32_t> stack;
    stack.push(m_root);
    
    while (!stack.empty()) {
        int32_t nodeId = stack.top();
        stack.pop();
        
        if (nodeId == NULL_PROXY) continue;
        
        const Node& node = m_nodes[nodeId];
        
        if (!Overlaps(min, max, node.aabbMin, node.aabbMax)) {
            continue;
        }
        
        if (node.IsLeaf()) {
            if (!callback(node.proxyIndex)) {
                return;
            }
        } else {
            stack.push(node.left);
            stack.push(node.right);
        }
    }
}

void PhysicsBVH::RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                         std::function<bool(ProxyID, float)> callback) const {
    if (m_root == NULL_PROXY) return;
    
    glm::vec3 invDir(
        std::abs(direction.x) > 1e-8f ? 1.0f / direction.x : 1e8f,
        std::abs(direction.y) > 1e-8f ? 1.0f / direction.y : 1e8f,
        std::abs(direction.z) > 1e-8f ? 1.0f / direction.z : 1e8f
    );
    
    std::stack<int32_t> stack;
    stack.push(m_root);
    
    while (!stack.empty()) {
        int32_t nodeId = stack.top();
        stack.pop();
        
        if (nodeId == NULL_PROXY) continue;
        
        const Node& node = m_nodes[nodeId];
        
        float t;
        if (!RayAABBIntersect(origin, invDir, node.aabbMin, node.aabbMax, t)) {
            continue;
        }
        
        if (t > maxDistance) continue;
        
        if (node.IsLeaf()) {
            if (!callback(node.proxyIndex, t)) {
                return;
            }
        } else {
            stack.push(node.left);
            stack.push(node.right);
        }
    }
}

const PhysicsBVH::Proxy* PhysicsBVH::GetProxy(ProxyID id) const {
    if (id <= 0 || id >= static_cast<ProxyID>(m_proxies.size())) return nullptr;
    return &m_proxies[id];
}

PhysicsBVH::Proxy* PhysicsBVH::GetProxy(ProxyID id) {
    if (id <= 0 || id >= static_cast<ProxyID>(m_proxies.size())) return nullptr;
    return &m_proxies[id];
}

int PhysicsBVH::GetHeight() const {
    if (m_root == NULL_PROXY) return 0;
    return m_nodes[m_root].height;
}

float PhysicsBVH::GetAreaRatio() const {
    if (m_root == NULL_PROXY) return 0.0f;
    
    float totalArea = 0.0f;
    float rootArea = m_nodes[m_root].GetSurfaceArea();
    
    for (size_t i = 0; i < m_nodes.size(); i++) {
        if (m_nodes[i].height >= 0) {
            totalArea += m_nodes[i].GetSurfaceArea();
        }
    }
    
    return totalArea / rootArea;
}

void PhysicsBVH::Clear() {
    m_nodes.clear();
    m_proxies.clear();
    m_proxyToNode.clear();
    m_root = NULL_PROXY;
    m_freeList = NULL_PROXY;
    m_freeCount = 0;
    m_proxyCount = 0;
}

bool PhysicsBVH::Validate() const {
    // Basic validation - check tree structure
    if (m_root == NULL_PROXY) return true;
    
    std::function<bool(int32_t)> validateNode = [&](int32_t nodeId) -> bool {
        if (nodeId == NULL_PROXY) return true;
        
        const Node& node = m_nodes[nodeId];
        
        if (node.IsLeaf()) {
            return node.proxyIndex != NULL_PROXY;
        }
        
        // Check children reference parent
        if (node.left != NULL_PROXY && m_nodes[node.left].parent != nodeId) return false;
        if (node.right != NULL_PROXY && m_nodes[node.right].parent != nodeId) return false;
        
        return validateNode(node.left) && validateNode(node.right);
    };
    
    return validateNode(m_root);
}

// ============== PRIVATE METHODS ==============

int32_t PhysicsBVH::AllocateNode() {
    if (m_freeList != NULL_PROXY) {
        int32_t nodeId = m_freeList;
        m_freeList = m_nodes[nodeId].parent;
        m_nodes[nodeId] = Node();
        m_freeCount--;
        return nodeId;
    }
    
    int32_t nodeId = static_cast<int32_t>(m_nodes.size());
    m_nodes.push_back(Node());
    return nodeId;
}

void PhysicsBVH::FreeNode(int32_t nodeId) {
    m_nodes[nodeId].parent = m_freeList;
    m_nodes[nodeId].height = -1;
    m_freeList = nodeId;
    m_freeCount++;
}

void PhysicsBVH::InsertLeaf(int32_t leafId) {
    if (m_root == NULL_PROXY) {
        m_root = leafId;
        m_nodes[m_root].parent = NULL_PROXY;
        return;
    }
    
    // Find best sibling using SAH
    Node& leafNode = m_nodes[leafId];
    glm::vec3 leafMin = leafNode.aabbMin;
    glm::vec3 leafMax = leafNode.aabbMax;
    
    int32_t sibling = m_root;
    while (!m_nodes[sibling].IsLeaf()) {
        const Node& node = m_nodes[sibling];
        
        float area = node.GetSurfaceArea();
        
        glm::vec3 combinedMin, combinedMax;
        Union(node.aabbMin, node.aabbMax, leafMin, leafMax, combinedMin, combinedMax);
        float combinedArea = SurfaceArea(combinedMin, combinedMax);
        
        float cost = 2.0f * combinedArea;
        float inheritanceCost = 2.0f * (combinedArea - area);
        
        // Cost of descending into children
        float costLeft, costRight;
        
        if (m_nodes[node.left].IsLeaf()) {
            glm::vec3 aabbMin, aabbMax;
            Union(m_nodes[node.left].aabbMin, m_nodes[node.left].aabbMax, leafMin, leafMax, aabbMin, aabbMax);
            costLeft = SurfaceArea(aabbMin, aabbMax) + inheritanceCost;
        } else {
            glm::vec3 aabbMin, aabbMax;
            Union(m_nodes[node.left].aabbMin, m_nodes[node.left].aabbMax, leafMin, leafMax, aabbMin, aabbMax);
            float oldArea = m_nodes[node.left].GetSurfaceArea();
            float newArea = SurfaceArea(aabbMin, aabbMax);
            costLeft = (newArea - oldArea) + inheritanceCost;
        }
        
        if (m_nodes[node.right].IsLeaf()) {
            glm::vec3 aabbMin, aabbMax;
            Union(m_nodes[node.right].aabbMin, m_nodes[node.right].aabbMax, leafMin, leafMax, aabbMin, aabbMax);
            costRight = SurfaceArea(aabbMin, aabbMax) + inheritanceCost;
        } else {
            glm::vec3 aabbMin, aabbMax;
            Union(m_nodes[node.right].aabbMin, m_nodes[node.right].aabbMax, leafMin, leafMax, aabbMin, aabbMax);
            float oldArea = m_nodes[node.right].GetSurfaceArea();
            float newArea = SurfaceArea(aabbMin, aabbMax);
            costRight = (newArea - oldArea) + inheritanceCost;
        }
        
        if (cost < costLeft && cost < costRight) {
            break;
        }
        
        sibling = (costLeft < costRight) ? node.left : node.right;
    }
    
    // Create new parent
    int32_t oldParent = m_nodes[sibling].parent;
    int32_t newParent = AllocateNode();
    
    Node& newParentNode = m_nodes[newParent];
    newParentNode.parent = oldParent;
    Union(leafMin, leafMax, m_nodes[sibling].aabbMin, m_nodes[sibling].aabbMax,
          newParentNode.aabbMin, newParentNode.aabbMax);
    newParentNode.height = m_nodes[sibling].height + 1;
    
    if (oldParent != NULL_PROXY) {
        if (m_nodes[oldParent].left == sibling) {
            m_nodes[oldParent].left = newParent;
        } else {
            m_nodes[oldParent].right = newParent;
        }
    } else {
        m_root = newParent;
    }
    
    newParentNode.left = sibling;
    newParentNode.right = leafId;
    m_nodes[sibling].parent = newParent;
    m_nodes[leafId].parent = newParent;
    
    // Walk back up fixing heights and balancing
    int32_t ancestor = newParent;
    while (ancestor != NULL_PROXY) {
        ancestor = Balance(ancestor);
        
        Node& ancestorNode = m_nodes[ancestor];
        int32_t left = ancestorNode.left;
        int32_t right = ancestorNode.right;
        
        ancestorNode.height = 1 + std::max(m_nodes[left].height, m_nodes[right].height);
        
        Union(m_nodes[left].aabbMin, m_nodes[left].aabbMax,
              m_nodes[right].aabbMin, m_nodes[right].aabbMax,
              ancestorNode.aabbMin, ancestorNode.aabbMax);
        
        ancestor = ancestorNode.parent;
    }
}

void PhysicsBVH::RemoveLeaf(int32_t leafId) {
    if (leafId == m_root) {
        m_root = NULL_PROXY;
        return;
    }
    
    int32_t parent = m_nodes[leafId].parent;
    int32_t grandParent = m_nodes[parent].parent;
    int32_t sibling = (m_nodes[parent].left == leafId) ? m_nodes[parent].right : m_nodes[parent].left;
    
    if (grandParent != NULL_PROXY) {
        if (m_nodes[grandParent].left == parent) {
            m_nodes[grandParent].left = sibling;
        } else {
            m_nodes[grandParent].right = sibling;
        }
        m_nodes[sibling].parent = grandParent;
        FreeNode(parent);
        
        // Fix ancestors
        int32_t ancestor = grandParent;
        while (ancestor != NULL_PROXY) {
            ancestor = Balance(ancestor);
            
            Node& ancestorNode = m_nodes[ancestor];
            int32_t left = ancestorNode.left;
            int32_t right = ancestorNode.right;
            
            Union(m_nodes[left].aabbMin, m_nodes[left].aabbMax,
                  m_nodes[right].aabbMin, m_nodes[right].aabbMax,
                  ancestorNode.aabbMin, ancestorNode.aabbMax);
            ancestorNode.height = 1 + std::max(m_nodes[left].height, m_nodes[right].height);
            
            ancestor = ancestorNode.parent;
        }
    } else {
        m_root = sibling;
        m_nodes[sibling].parent = NULL_PROXY;
        FreeNode(parent);
    }
}

int32_t PhysicsBVH::Balance(int32_t nodeId) {
    Node& A = m_nodes[nodeId];
    
    if (A.IsLeaf() || A.height < 2) {
        return nodeId;
    }
    
    int32_t iB = A.left;
    int32_t iC = A.right;
    Node& B = m_nodes[iB];
    Node& C = m_nodes[iC];
    
    int balance = C.height - B.height;
    
    // Rotate C up
    if (balance > 1) {
        int32_t iF = C.left;
        int32_t iG = C.right;
        Node& F = m_nodes[iF];
        Node& G = m_nodes[iG];
        
        C.left = nodeId;
        C.parent = A.parent;
        A.parent = iC;
        
        if (C.parent != NULL_PROXY) {
            if (m_nodes[C.parent].left == nodeId) {
                m_nodes[C.parent].left = iC;
            } else {
                m_nodes[C.parent].right = iC;
            }
        } else {
            m_root = iC;
        }
        
        if (F.height > G.height) {
            C.right = iF;
            A.right = iG;
            G.parent = nodeId;
            
            Union(B.aabbMin, B.aabbMax, G.aabbMin, G.aabbMax, A.aabbMin, A.aabbMax);
            Union(A.aabbMin, A.aabbMax, F.aabbMin, F.aabbMax, C.aabbMin, C.aabbMax);
            
            A.height = 1 + std::max(B.height, G.height);
            C.height = 1 + std::max(A.height, F.height);
        } else {
            C.right = iG;
            A.right = iF;
            F.parent = nodeId;
            
            Union(B.aabbMin, B.aabbMax, F.aabbMin, F.aabbMax, A.aabbMin, A.aabbMax);
            Union(A.aabbMin, A.aabbMax, G.aabbMin, G.aabbMax, C.aabbMin, C.aabbMax);
            
            A.height = 1 + std::max(B.height, F.height);
            C.height = 1 + std::max(A.height, G.height);
        }
        
        return iC;
    }
    
    // Rotate B up
    if (balance < -1) {
        int32_t iD = B.left;
        int32_t iE = B.right;
        Node& D = m_nodes[iD];
        Node& E = m_nodes[iE];
        
        B.left = nodeId;
        B.parent = A.parent;
        A.parent = iB;
        
        if (B.parent != NULL_PROXY) {
            if (m_nodes[B.parent].left == nodeId) {
                m_nodes[B.parent].left = iB;
            } else {
                m_nodes[B.parent].right = iB;
            }
        } else {
            m_root = iB;
        }
        
        if (D.height > E.height) {
            B.right = iD;
            A.left = iE;
            E.parent = nodeId;
            
            Union(C.aabbMin, C.aabbMax, E.aabbMin, E.aabbMax, A.aabbMin, A.aabbMax);
            Union(A.aabbMin, A.aabbMax, D.aabbMin, D.aabbMax, B.aabbMin, B.aabbMax);
            
            A.height = 1 + std::max(C.height, E.height);
            B.height = 1 + std::max(A.height, D.height);
        } else {
            B.right = iE;
            A.left = iD;
            D.parent = nodeId;
            
            Union(C.aabbMin, C.aabbMax, D.aabbMin, D.aabbMax, A.aabbMin, A.aabbMax);
            Union(A.aabbMin, A.aabbMax, E.aabbMin, E.aabbMax, B.aabbMin, B.aabbMax);
            
            A.height = 1 + std::max(C.height, D.height);
            B.height = 1 + std::max(A.height, E.height);
        }
        
        return iB;
    }
    
    return nodeId;
}

glm::vec3 PhysicsBVH::Union(const glm::vec3& minA, const glm::vec3& maxA,
                            const glm::vec3& minB, const glm::vec3& maxB,
                            glm::vec3& outMin, glm::vec3& outMax) {
    outMin = glm::min(minA, minB);
    outMax = glm::max(maxA, maxB);
    return outMax - outMin;
}

float PhysicsBVH::SurfaceArea(const glm::vec3& min, const glm::vec3& max) {
    glm::vec3 d = max - min;
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

bool PhysicsBVH::Overlaps(const glm::vec3& minA, const glm::vec3& maxA,
                          const glm::vec3& minB, const glm::vec3& maxB) {
    return !(maxA.x < minB.x || minA.x > maxB.x ||
             maxA.y < minB.y || minA.y > maxB.y ||
             maxA.z < minB.z || minA.z > maxB.z);
}

bool PhysicsBVH::RayAABBIntersect(const glm::vec3& origin, const glm::vec3& invDir,
                                   const glm::vec3& min, const glm::vec3& max,
                                   float& tMin) const {
    glm::vec3 t0 = (min - origin) * invDir;
    glm::vec3 t1 = (max - origin) * invDir;
    
    glm::vec3 tNear = glm::min(t0, t1);
    glm::vec3 tFar = glm::max(t0, t1);
    
    tMin = std::max({tNear.x, tNear.y, tNear.z});
    float tMax = std::min({tFar.x, tFar.y, tFar.z});
    
    return tMax >= tMin && tMax >= 0.0f;
}
