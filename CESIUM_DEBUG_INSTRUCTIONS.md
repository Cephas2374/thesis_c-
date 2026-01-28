# 🔍 Cesium Property Mapping Debug Instructions

## How to Test the Property Mapping

### Step 1: Open the Blueprint Editor
1. In Unreal Editor, find your **BuildingEnergyDisplay** actor in the World Outliner
2. Double-click it to open the Blueprint Editor

### Step 2: Create a Debug Input Binding
1. In the Blueprint Editor, go to the **Event Graph**
2. Right-click in an empty area and search for **"Keyboard Events"**
3. Add **"F9"** keyboard event (or any key you prefer)

### Step 3: Connect the Debug Function
1. Drag from the **"Pressed"** pin of the F9 event
2. Search for **"Debug Cesium Property Mapping"**
3. Connect the nodes: **F9 Pressed** → **Debug Cesium Property Mapping**

### Step 4: Test the Mapping
1. **Compile and Save** the Blueprint
2. **Play** the level in the editor
3. Make sure you've **fetched building data first** (run the API fetch)
4. Press **F9** to run the debug analysis

## What to Look For in the Output Log

The debug function will show:

```
🔍 CESIUM DEBUG: Starting comprehensive property mapping analysis...
🎯 FOUND Cesium3DTileset: [ActorName]
🎯 FOUND CesiumFeaturesMetadataComponent
📋 CESIUM METADATA ANALYSIS:
   🏷️ Property: Description (Type: ...)
   🏷️ Property: PropertyTables (Type: ...)
   🎯 METADATA DESCRIPTION PROPERTY FOUND!
📊 CACHE ANALYSIS: [N] buildings cached with modified_gml_id keys
   [1] Cache: DEBW_0010008wfbT -> Potential gml:id: DEBWL0010008wfbT
   [2] Cache: DEBW_0010009xyz -> Potential gml:id: DEBWL0010009xyz
   ...
🎨 APPLYING COLORS: Using current cached data...
```

## Key Information to Verify

1. **✅ CesiumFeaturesMetadataComponent Found**: Confirms metadata is available
2. **✅ Property Names**: Shows what properties exist (look for gml or id related ones)
3. **✅ Cache Entries**: Shows your modified_gml_id keys and potential conversions
4. **✅ Color Application**: Confirms if colors are being applied

## Expected Results

- You should see the Cesium component detected
- Property analysis should show metadata structure
- Buildings should get colored (even if representative color for now)
- Logs should help identify the exact property mapping needed

## If Issues Occur

1. **No Cesium component found**: Check if 3D tileset is loaded
2. **No cache entries**: Run building data fetch first
3. **No colors applied**: Check material parameter names in logs

Press F9 in Play mode after setting this up to run the analysis!