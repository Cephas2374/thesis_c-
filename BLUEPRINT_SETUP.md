## 🎮 Blueprint Setup for Cesium Property Debug

### Alternative 1: Input Event (Recommended)
```
[F9 Key Event] → [Debug Cesium Property Mapping]
```

### Alternative 2: BeginPlay Event (Auto-run)
```
[BeginPlay] → [Delay: 5.0] → [Debug Cesium Property Mapping]
```

### Alternative 3: Custom Event
```
[Custom Event: "RunCesiumDebug"] → [Debug Cesium Property Mapping]
```

## Quick Test Steps:

1. **Open Blueprint**: Double-click BuildingEnergyDisplay in World Outliner
2. **Add F9 Event**: Right-click → Input → Keyboard Events → F9
3. **Connect Function**: F9 Pressed → Debug Cesium Property Mapping  
4. **Compile & Save**
5. **Play Level**
6. **Fetch Building Data** (if not done already)
7. **Press F9** to run analysis

## Expected Debug Output:

The function will log comprehensive information about:
- Cesium tileset detection ✅
- Metadata component analysis ✅  
- Available properties in Cesium ✅
- Cache entries with modified_gml_id ✅
- Potential gml:id conversions ✅
- Color application results ✅

This will help identify exactly how to map between Cesium's `gml:id` and our `modified_gml_id` cache keys!