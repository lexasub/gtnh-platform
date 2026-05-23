# Task: SpatialIndex Integration

## Overview
Implement SpatialIndex service (R-tree/Octree) for efficient multiblock pattern detection during block placement and changes. This is a critical dependency for L2 multiblock gameplay.

## Goal
- Implement fully functional SpatialIndex service with R-tree query capabilities
- Enable efficient pattern detection during block changes
- Support radius queries, pattern matching, and adjacency queries

## Acceptance Criteria
- [ ] SpatialIndex service builds and runs
- [ ] R-tree implemented via Boost.Geometry (bgi::rtree<AABB>)
- [ ] Query API: findBlocksInRadius(), isPatternComplete(), findAdjacent()
- [ ] MessageRouter integration with topics: spatial.query.radius, spatial.query.pattern
- [ ] Performance: O(log n) queries for pattern detection
- [ ] Tested with 10k+ blocks without performance degradation

## Requirements

### Technical Requirements
- **Language**: C++
- **Library**: Boost.Geometry for R-tree implementation
- **Service Type**: MessageRouter service (Go sidecar integration)
- **Performance**: Sub-millisecond queries for 10k+ blocks
- **Memory**: Efficient node representation for chunk coordinates (32-bit)

### API Design
```cpp
class SpatialIndex {
public:
    // Insert block position with metadata
    void Insert(uint32_t x, uint32_t y, uint32_t z, uint64_t mb_id = 0);
    
    // Query blocks within radius (in blocks)
    std::vector<BlockPos> FindBlocksInRadius(uint32_t x, uint32_t y, uint32_t z, uint32_t radius);
    
    // Check if pattern can be completed at position
    bool IsPatternComplete(const PatternLayer& pattern, uint32_t x, uint32_t y, uint32_t z);
    
    // Find adjacent blocks for pattern completion
    std::vector<BlockPos> FindAdjacentBlocks(const PatternLayer& pattern, uint32_t x, uint32_t y, uint32_t z);
    
    // Remove block on destruction
    void Remove(uint32_t x, uint32_t y, uint32_t z);
};
```

### MessageRouter Integration
- **Topics**:
  - `spatial.query.radius` - radius-based block queries
  - `spatial.query.pattern` - pattern completion checks
  - `spatial.insert` - block insertion events
  - `spatial.remove` - block removal events

- **Message Types**:
  - `SpatialQueryRequest` - contains query type and parameters
  - `SpatialQueryResponse` - contains query results
  - `SpatialUpdate` - block position changes

### Performance Considerations
- **Node representation**: 32-bit coordinates (max world size)
- **Bounding boxes**: Axis-aligned for efficient intersection tests
- **Query optimization**: Pre-filter with coarse grid, fine with R-tree
- **Memory layout**: Contiguous storage for cache efficiency

## Dependencies
- **Required**: Boost.Geometry, SpatialIndex headers
- **Optional**: Custom AABB implementation if Boost unavailable

## Risk Assessment
- **High**: Boost dependency may require specific version
- **Medium**: Query accuracy must match game mechanics
- **Low**: Performance targets achievable with proper optimization

## Testing Strategy
- Unit tests for query accuracy
- Performance benchmarks with world generation
- Integration tests with SimulationCore
- Stress tests with 50k+ blocks

## Implementation Timeline
- **Week 1**: R-tree core implementation
- **Week 2**: Query API and MessageRouter integration
- **Week 3**: Performance optimization and testing
- **Week 4**: Integration testing and final verification

## Alternative Approach (Deferred)
If SpatialIndex proves too complex or time-consuming:
1. Use direct ChunkStore.GetBlock() queries for pattern detection
2. O(blocks_in_pattern) complexity (36 queries for 3×3×4 pattern)
3. Acceptable for L2 MVP, defer SpatialIndex to L3
4. Trade performance for faster implementation

## Files to Modify
- `src/services/spatial_index/main.cpp` - Service implementation
- `src/services/spatial_index/include/spatial_index.hpp` - Public API
- `src/services/spatial_index/src/*.cpp` - Implementation files
- `src/services/message_router/router_client.go` - Go client integration

## Success Metrics
- Query response time < 1ms for 10k blocks
- Pattern detection accuracy 100%
- Memory usage < 100MB for world-sized index
- No memory leaks under stress tests