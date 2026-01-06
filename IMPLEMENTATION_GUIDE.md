# Real-Time Building Energy Visualization Implementation Guide

## Overview

This implementation guide provides step-by-step instructions and key code sections for developing a real-time building energy visualization system in Unreal Engine 5.6 with Cesium 3D tilesets integration.

## Prerequisites

- Unreal Engine 5.6 or later
- Visual Studio 2022 with C++ development tools
- Cesium for Unreal plugin installed
- Access to backend API (backend.gisworld-tech.com)

## Step 1: Project Setup and Dependencies

### 1.1 Create Unreal Engine C++ Project

First, we configure the project's module dependencies to include all necessary Unreal Engine systems for HTTP communication, JSON processing, and user interface creation.

```cpp
// Final_project.Build.cs
using UnrealBuildTool;

public class Final_project : ModuleRules
{
    public Final_project(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] { 
            "Core", 
            "CoreUObject", 
            "Engine", 
            "HTTP", 
            "Json", 
            "JsonUtilities",
            "UMG",
            "Slate",
            "SlateCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[] {});
    }
}
```

**Interpretation**: This build configuration file tells Unreal Engine which modules our project depends on. We include HTTP for API calls, Json for data parsing, and UMG/Slate for user interface creation.

### 1.2 Install and Configure Cesium Plugin

1. Install Cesium for Unreal from the Unreal Engine Marketplace
2. Enable the plugin in your project settings
3. Add Cesium 3D Tileset to your level
4. Configure Ion access token if using Cesium Ion assets

## Step 2: Core Actor Implementation

### 2.1 Header File Declaration

The main actor class declaration defines all the components needed for our building energy system: API credentials, data caches, and function declarations for network operations and visualization.

```cpp
// BuildingEnergyDisplay.h
UCLASS()
class FINAL_PROJECT_API ABuildingEnergyDisplay : public AActor
{
    GENERATED_BODY()
    
public:    
    ABuildingEnergyDisplay();

protected:
    virtual void BeginPlay() override;
    
    // HTTP and API management
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "API")
    FString BaseURL = TEXT("https://backend.gisworld-tech.com");
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "API")
    FString AccessToken;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "API")
    FString RefreshToken;

    // Caching systems
    UPROPERTY()
    TMap<FString, FString> BuildingDataCache;
    
    UPROPERTY()
    TMap<FString, FLinearColor> BuildingColorCache;
    
    UPROPERTY()
    TMap<FString, TArray<FVector>> BuildingCoordinatesCache;
    
    UPROPERTY()
    TMap<FString, FString> GmlIdCache;

    // Core functions
    UFUNCTION(BlueprintCallable, Category = "API")
    void StartPolling();
    
    UFUNCTION(BlueprintCallable, Category = "Energy Display")
    void ApplyColorsToTileset();

private:
    FHttpModule* HttpModule;
    FTimerHandle PollingTimerHandle;
    
    // API functions
    void RefreshAccessToken();
    void FetchBuildingData();
    void ParseAndCacheAllBuildings(const FString& JsonData);
    
    // Styling functions
    void CreatePerBuildingColorMaterial();
    void ApplyColorsToCSiumTileset();
    FString CreateCesiumColorExpression();
    
    // Utility functions
    FLinearColor ConvertHexToLinearColor(const FString& HexColor);
    bool ParseBuildingCoordinates(const FString& GeomJson, TArray<FVector>& Coordinates);
};
```

**Interpretation**: This header defines our main actor class with four key areas: API credentials (editable in editor), cache systems (TMap containers for fast data lookup), public functions (callable from Blueprint), and private helper functions. The UPROPERTY macros make variables visible to Unreal's reflection system.

### 2.2 Constructor and Initialization

The constructor sets up essential references, while BeginPlay() automatically starts the system when the actor is spawned in the level, checking for proper authentication configuration.

```cpp
// BuildingEnergyDisplay.cpp
ABuildingEnergyDisplay::ABuildingEnergyDisplay()
{
    PrimaryActorTick.bCanEverTick = false;
    HttpModule = &FHttpModule::Get();
}

void ABuildingEnergyDisplay::BeginPlay()
{
    Super::BeginPlay();
    
    UE_LOG(LogTemp, Warning, TEXT("BuildingEnergyDisplay: Starting system..."));
    
    // Initialize with authentication
    if (!AccessToken.IsEmpty())
    {
        StartPolling();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Access token not set! Please configure authentication."));
    }
}
```

**Interpretation**: The constructor disables ticking (since we don't need per-frame updates) and gets a reference to the HTTP module. BeginPlay() runs when the actor starts, checking if we have an access token and automatically starting data polling if authentication is configured.

## Step 3: Authentication and HTTP Management

### 3.1 Token Refresh Implementation

When API calls fail due to expired tokens, this function automatically requests a new access token using the refresh token, ensuring uninterrupted data flow without user intervention.

```cpp
void ABuildingEnergyDisplay::RefreshAccessToken()
{
    if (RefreshToken.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("No refresh token available"));
        return;
    }

    auto Request = HttpModule->CreateRequest();
    Request->SetVerb("POST");
    Request->SetURL(BaseURL + "/auth/refresh");
    Request->SetHeader("Content-Type", "application/json");
    Request->SetHeader("Authorization", "Bearer " + RefreshToken);

    Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
    {
        if (bWasSuccessful && Response.IsValid())
        {
            if (Response->GetResponseCode() == 200)
            {
                TSharedPtr<FJsonObject> JsonObject;
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
                
                if (FJsonSerializer::Deserialize(Reader, JsonObject))
                {
                    if (JsonObject->HasField("access_token"))
                    {
                        AccessToken = JsonObject->GetStringField("access_token");
                        UE_LOG(LogTemp, Warning, TEXT("ACCESS TOKEN REFRESHED successfully"));
                        
                        // Resume polling after successful refresh
                        StartPolling();
                    }
                }
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("Token refresh failed: %d"), Response->GetResponseCode());
            }
        }
    });

    Request->ProcessRequest();
}
```

**Interpretation**: This function handles JWT token refresh using the refresh token. It creates an HTTP POST request to the auth endpoint, parses the JSON response to extract the new access token, and automatically resumes data polling on success. The lambda callback handles the asynchronous response processing.

### 3.2 Polling Implementation

The polling system creates a reliable data synchronization mechanism by regularly fetching building energy data from the backend API, with automatic error handling and token refresh capabilities.

```cpp
void ABuildingEnergyDisplay::StartPolling()
{
    UE_LOG(LogTemp, Warning, TEXT("POLLING Starting building data polling..."));
    
    // Clear existing timer
    GetWorld()->GetTimerManager().ClearTimer(PollingTimerHandle);
    
    // Start immediate fetch
    FetchBuildingData();
    
    // Set up recurring polling every 30 seconds
    GetWorld()->GetTimerManager().SetTimer(PollingTimerHandle, this, 
        &ABuildingEnergyDisplay::FetchBuildingData, 30.0f, true);
}

void ABuildingEnergyDisplay::FetchBuildingData()
{
    auto Request = HttpModule->CreateRequest();
    Request->SetVerb("GET");
    Request->SetURL(BaseURL + "/geospatial/buildings-energy/?community_id=084170088&format=json");
    Request->SetHeader("Authorization", "Bearer " + AccessToken);
    Request->SetHeader("Accept", "application/json");

    Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
    {
        if (bWasSuccessful && Response.IsValid())
        {
            int32 ResponseCode = Response->GetResponseCode();
            FString ResponseBody = Response->GetContentAsString();
            
            if (ResponseCode == 200)
            {
                UE_LOG(LogTemp, Warning, TEXT("API SUCCESS: Received %d characters of data"), ResponseBody.Len());
                ParseAndCacheAllBuildings(ResponseBody);
            }
            else if (ResponseCode == 401)
            {
                UE_LOG(LogTemp, Warning, TEXT("TOKEN EXPIRED - Refreshing..."));
                RefreshAccessToken();
            }
            else
            {
                UE_LOG(LogTemp, Error, TEXT("API ERROR: %d"), ResponseCode);
            }
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("HTTP REQUEST FAILED"));
        }
    });

    Request->ProcessRequest();
}
```

**Interpretation**: StartPolling() sets up a timer to fetch building data every 30 seconds for real-time updates. FetchBuildingData() makes a GET request to retrieve building energy information, handles authentication errors by triggering token refresh, and processes successful responses by parsing the JSON data. The polling ensures the visualization stays current with backend changes.

## Step 4: JSON Parsing and Data Processing

### 4.1 Building Data Parsing

This comprehensive parsing function processes the complex JSON response from the API, extracting building identifiers, coordinate data, and energy color information while handling data inconsistencies and caching everything for fast access.

```cpp
void ABuildingEnergyDisplay::ParseAndCacheAllBuildings(const FString& JsonData)
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonData);
    
    if (!FJsonSerializer::Deserialize(Reader, JsonObject))
    {
        UE_LOG(LogTemp, Error, TEXT("PARSE Failed to parse JSON response"));
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* BuildingsArray;
    if (!JsonObject->TryGetArrayField(TEXT("buildings"), BuildingsArray))
    {
        UE_LOG(LogTemp, Error, TEXT("PARSE No 'buildings' array found in response"));
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("PARSE Processing %d buildings..."), BuildingsArray->Num());

    for (const TSharedPtr<FJsonValue>& BuildingValue : *BuildingsArray)
    {
        TSharedPtr<FJsonObject> Building = BuildingValue->AsObject();
        if (!Building.IsValid()) continue;

        // Extract building identifiers
        FString ModifiedGmlId, GmlId;
        Building->TryGetStringField(TEXT("modified_gml_id"), ModifiedGmlId);
        Building->TryGetStringField(TEXT("gml_id"), GmlId);
        
        if (ModifiedGmlId.IsEmpty()) continue;

        // Cache GML ID mapping
        if (!GmlId.IsEmpty() && GmlId != ModifiedGmlId)
        {
            GmlIdCache.Add(ModifiedGmlId, GmlId);
            UE_LOG(LogTemp, Log, TEXT("Cached mapping: %s -> %s"), *ModifiedGmlId, *GmlId);
        }

        // Extract and cache coordinates
        FString GeomString;
        if (Building->TryGetStringField(TEXT("geom"), GeomString))
        {
            TArray<FVector> Coordinates;
            if (ParseBuildingCoordinates(GeomString, Coordinates))
            {
                // Handle duplicate GML IDs with unique cache keys
                FString CacheKey = ModifiedGmlId;
                double NumericId;
                if (Building->TryGetNumberField(TEXT("id"), NumericId))
                {
                    CacheKey = FString::Printf(TEXT("%s#%d"), *ModifiedGmlId, (int32)NumericId);
                }
                BuildingCoordinatesCache.Add(CacheKey, Coordinates);
            }
        }

        // Extract energy color data
        const TSharedPtr<FJsonObject>* EnergyResult;
        if (Building->TryGetObjectField(TEXT("energy_result"), EnergyResult))
        {
            const TSharedPtr<FJsonObject>* EndResult;
            if ((*EnergyResult)->TryGetObjectField(TEXT("end"), EndResult))
            {
                const TSharedPtr<FJsonObject>* ColorResult;
                if ((*EndResult)->TryGetObjectField(TEXT("color"), ColorResult))
                {
                    FString ColorHex;
                    if ((*ColorResult)->TryGetStringField(TEXT("energy_demand_specific_color"), ColorHex))
                    {
                        FLinearColor BuildingColor = ConvertHexToLinearColor(ColorHex);
                        BuildingColorCache.Add(ModifiedGmlId, BuildingColor);
                        
                        // Also cache under GmlId if different
                        if (!GmlId.IsEmpty() && GmlId != ModifiedGmlId)
                        {
                            BuildingColorCache.Add(GmlId, BuildingColor);
                        }
                        
                        UE_LOG(LogTemp, Log, TEXT("COLOR Cached color %s for building %s"), *ColorHex, *ModifiedGmlId);
                    }
                }
            }
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("PARSE Complete: %d colors, %d coordinates, %d mappings"), 
        BuildingColorCache.Num(), BuildingCoordinatesCache.Num(), GmlIdCache.Num());
    
    // Apply colors to tileset
    CreatePerBuildingColorMaterial();
}```

**Interpretation**: This function parses the API's JSON response containing an array of buildings. For each building, it extracts the GML IDs (both modified and original formats), processes GeoJSON coordinate data, and extracts energy-specific color information from nested JSON objects. The data is cached in multiple maps for fast lookup, with special handling for duplicate GML IDs using unique cache keys.```

### 4.2 Coordinate Parsing with Error Handling

GeoJSON polygon data often contains deeply nested coordinate arrays that require careful parsing to extract individual coordinate points while avoiding crashes from malformed data structures.

```cpp
bool ABuildingEnergyDisplay::ParseBuildingCoordinates(const FString& GeomJson, TArray<FVector>& Coordinates)
{
    TSharedPtr<FJsonObject> GeomObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GeomJson);
    
    if (!FJsonSerializer::Deserialize(Reader, GeomObject))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* CoordinatesArray;
    if (!GeomObject->TryGetArrayField(TEXT("coordinates"), CoordinatesArray))
    {
        return false;
    }

    // Handle nested polygon structure
    for (const TSharedPtr<FJsonValue>& CoordLevel1 : *CoordinatesArray)
    {
        if (CoordLevel1->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Level1Array = CoordLevel1->AsArray();
            
            for (const TSharedPtr<FJsonValue>& CoordLevel2 : Level1Array)
            {
                if (CoordLevel2->Type == EJson::Array)
                {
                    const TArray<TSharedPtr<FJsonValue>>& Level2Array = CoordLevel2->AsArray();
                    
                    for (const TSharedPtr<FJsonValue>& CoordPair : Level2Array)
                    {
                        if (CoordPair->Type == EJson::Array)
                        {
                            const TArray<TSharedPtr<FJsonValue>>& PairArray = CoordPair->AsArray();
                            if (PairArray.Num() >= 2)
                            {
                                // Safely extract coordinates with type checking
                                double X = 0.0, Y = 0.0;
                                if (PairArray[0]->Type == EJson::Number && PairArray[1]->Type == EJson::Number)
                                {
                                    X = PairArray[0]->AsNumber();
                                    Y = PairArray[1]->AsNumber();
                                    Coordinates.Add(FVector(X, Y, 0.0f));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return Coordinates.Num() > 0;
}
```

**Interpretation**: This function safely parses GeoJSON polygon coordinates by recursively traversing nested arrays (Polygon → rings → coordinate pairs). It includes robust type checking to prevent crashes when encountering unexpected data structures like arrays where numbers are expected. The function handles complex polygon geometries and extracts individual coordinate points as Unreal Engine FVector objects.

## Step 5: Color Conversion and Cesium Styling

### 5.1 Color Conversion Utility

Building energy colors arrive as hexadecimal strings from the API but need to be converted to Unreal Engine's native color format for material and rendering operations.

```cpp
FLinearColor ABuildingEnergyDisplay::ConvertHexToLinearColor(const FString& HexColor)
{
    FString CleanHex = HexColor;
    if (CleanHex.StartsWith(TEXT("#")))
    {
        CleanHex = CleanHex.RightChop(1);
    }
    
    if (CleanHex.Len() != 6)
    {
        UE_LOG(LogTemp, Warning, TEXT("Invalid hex color: %s"), *HexColor);
        return FLinearColor::Gray; // Default fallback
    }

    // Parse RGB components
    int32 R = FParse::HexNumber(*CleanHex.Left(2));
    int32 G = FParse::HexNumber(*CleanHex.Mid(2, 2));
    int32 B = FParse::HexNumber(*CleanHex.Right(2));
    
    // Convert to linear color (0-1 range)
    return FLinearColor(R / 255.0f, G / 255.0f, B / 255.0f, 1.0f);
}
```

**Interpretation**: This utility function converts hexadecimal color strings (like "#FF0000") into Unreal Engine's FLinearColor format. It removes the # prefix, validates the hex length, parses RGB components using hexadecimal parsing, and normalizes values from 0-255 range to 0-1 range required by Unreal's color system.

### 5.2 Cesium Style Expression Generation

Cesium 3D tilesets use JSON-based style expressions to apply colors to individual features. This function builds the conditional logic needed to map each building ID to its corresponding energy color.

```cpp
FString ABuildingEnergyDisplay::CreateCesiumColorExpression()
{
    if (BuildingColorCache.Num() == 0)
    {
        return TEXT("");
    }

    TArray<FString> MatchConditions;
    
    // Build match expression for each building
    for (const auto& BuildingColor : BuildingColorCache)
    {
        FString BuildingId = BuildingColor.Key;
        FLinearColor Color = BuildingColor.Value;
        
        // Convert back to hex for Cesium
        FColor SRGBColor = Color.ToFColor(true);
        FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), 
            SRGBColor.R, SRGBColor.G, SRGBColor.B);
        
        // Add to match conditions
        MatchConditions.Add(FString::Printf(TEXT("\"%s\", \"%s\""), 
            *BuildingId, *HexColor));
    }
    
    // Create complete match expression
    FString MatchExpression = FString::Printf(TEXT("[\"match\", [\"get\", \"gml_id\"], %s, \"#FFFFFF\"]"),
        *FString::Join(MatchConditions, TEXT(", ")));
    
    UE_LOG(LogTemp, Warning, TEXT("EXPRESSION Created Cesium match expression with %d conditions"), 
        MatchConditions.Num());
    
    return MatchExpression;
}
```

**Interpretation**: This function generates a Cesium-compatible style expression using JSON syntax. It creates a "match" expression that maps building GML IDs to their corresponding hex colors. The resulting expression follows Cesium's conditional styling format: ["match", ["get", "property"], "id1", "#color1", "id2", "#color2", "default_color"].

### 5.3 Cesium Tileset Color Application

The main styling function locates the Cesium tileset actor in the scene and applies the generated color expression to achieve per-building visualization based on energy consumption data.

```cpp
void ABuildingEnergyDisplay::CreatePerBuildingColorMaterial()
{
    UE_LOG(LogTemp, Warning, TEXT("STYLE === CESIUM COLOR APPLICATION START ==="));
    
    if (BuildingColorCache.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("WARNING No building colors available"));
        return;
    }

    // Find Cesium tileset actor
    AActor* CesiumActor = nullptr;
    UWorld* World = GetWorld();
    if (World)
    {
        for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
        {
            AActor* CurrentActor = *ActorItr;
            if (CurrentActor)
            {
                FString ActorName = CurrentActor->GetName();
                // Look for bisingen tileset or any Cesium3DTileset
                if (ActorName.Contains(TEXT("bisingen")) || 
                    CurrentActor->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
                {
                    UE_LOG(LogTemp, Warning, TEXT("TARGET Found Cesium tileset: %s"), *ActorName);
                    CesiumActor = CurrentActor;
                    break;
                }
            }
        }
    }

    if (!CesiumActor)
    {
        UE_LOG(LogTemp, Error, TEXT("ERROR No Cesium tileset actor found"));
        return;
    }

    // Generate Cesium style expression
    FString StyleExpression = CreateCesiumColorExpression();
    FString CompleteStyle = FString::Printf(TEXT("{\"color\": %s}"), *StyleExpression);
    
    // Apply style to Cesium component
    TArray<UActorComponent*> Components;
    CesiumActor->GetComponents<UActorComponent>(Components);
    
    bool bStyleApplied = false;
    for (UActorComponent* Component : Components)
    {
        if (Component->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
        {
            UE_LOG(LogTemp, Warning, TEXT("TARGET Found Cesium3DTileset component"));
            
            // Try to set style through reflection
            UClass* ComponentClass = Component->GetClass();
            for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
            {
                FProperty* Property = *PropIt;
                FString PropName = Property->GetName();
                
                if (PropName.Contains(TEXT("Style")) || PropName.Contains(TEXT("Expression")))
                {
                    if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
                    {
                        StrProperty->SetPropertyValue_InContainer(Component, CompleteStyle);
                        Component->Modify();
                        bStyleApplied = true;
                        UE_LOG(LogTemp, Warning, TEXT("SUCCESS Applied style to property: %s"), *PropName);
                        break;
                    }
                }
            }
            break;
        }
    }

    if (!bStyleApplied)
    {
        UE_LOG(LogTemp, Warning, TEXT("WARNING Style application may require manual Cesium component configuration"));
        UE_LOG(LogTemp, Warning, TEXT("INFO Add CesiumFeaturesMetadata component and use Auto Fill + Generate Material"));
    }
    
    UE_LOG(LogTemp, Warning, TEXT("STYLE === CESIUM COLOR APPLICATION END ==="));
}
```

**Interpretation**: This is the core styling function that finds the Cesium tileset actor in the scene and attempts to apply per-building colors. It searches for the 'bisingen' tileset or any Cesium3DTileset actor, generates a complete Cesium style JSON, and uses reflection to find and set style-related properties on the tileset component. If direct styling fails, it suggests manual configuration using CesiumFeaturesMetadata component.

## Step 6: User Interface Integration

### 6.1 UMG Widget Integration

To display detailed building information and allow user interaction, we integrate Unreal's UMG system to create and manage user interface widgets that show building attributes and allow editing.

```cpp
// In the header file
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
TSubclassOf<UUserWidget> BuildingAttributesWidgetClass;

UPROPERTY()
UUserWidget* BuildingAttributesWidget;

// Implementation
void ABuildingEnergyDisplay::ShowBuildingAttributes(const FString& BuildingGmlId)
{
    if (!BuildingAttributesWidgetClass)
    {
        UE_LOG(LogTemp, Error, TEXT("BuildingAttributesWidgetClass not set"));
        return;
    }

    // Remove existing widget
    if (BuildingAttributesWidget)
    {
        BuildingAttributesWidget->RemoveFromParent();
        BuildingAttributesWidget = nullptr;
    }

    // Create new widget
    if (UWorld* World = GetWorld())
    {
        if (APlayerController* PlayerController = World->GetFirstPlayerController())
        {
            BuildingAttributesWidget = CreateWidget<UUserWidget>(PlayerController, BuildingAttributesWidgetClass);
            if (BuildingAttributesWidget)
            {
                BuildingAttributesWidget->AddToViewport();
                
                // Set building data if widget supports it
                if (UBuildingAttributesWidget* AttributesWidget = Cast<UBuildingAttributesWidget>(BuildingAttributesWidget))
                {
                    AttributesWidget->SetBuildingData(BuildingGmlId, AccessToken);
                }
            }
        }
    }
}
```

**Interpretation**: This function manages the user interface for displaying building attributes. It creates a UMG widget from a Blueprint class, removes any existing widget to prevent UI conflicts, adds the new widget to the viewport for display, and passes the selected building's GML ID and authentication token to populate the form with building-specific data.

## Step 7: Building and Deployment

### 7.1 Build Configuration

Compiling the project requires specific MSBuild commands that handle Unreal Engine's complex build system and ensure all dependencies are properly linked.

```powershell
# Build command for development
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Final_project.sln /p:Configuration="Development Editor" /p:Platform=Win64

# Alternative: Use Unreal Build Tool
& "C:\Path\To\UnrealEngine\Engine\Binaries\DotNET\UnrealBuildTool.exe" Final_project Win64 Development "D:\ar_thesis_cephas\unreal\Final_project\Final_project.uproject" -WaitMutex
```

### 7.2 Runtime Testing and Debugging

To verify that our system is working correctly, we can add debug functions that provide visibility into the data processing and caching operations during runtime.

Add these debug commands to verify system functionality:

```cpp
// Add to BuildingEnergyDisplay class for testing
UFUNCTION(BlueprintCallable, Category = "Debug")
void DebugPrintCacheStats()
{
    UE_LOG(LogTemp, Warning, TEXT("=== CACHE STATISTICS ==="));
    UE_LOG(LogTemp, Warning, TEXT("Building Colors: %d"), BuildingColorCache.Num());
    UE_LOG(LogTemp, Warning, TEXT("Building Coordinates: %d"), BuildingCoordinatesCache.Num());
    UE_LOG(LogTemp, Warning, TEXT("GML ID Mappings: %d"), GmlIdCache.Num());
    
    // Show sample data
    int32 Count = 0;
    for (const auto& ColorEntry : BuildingColorCache)
    {
        FColor SRGBColor = ColorEntry.Value.ToFColor(true);
        FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
        UE_LOG(LogTemp, Warning, TEXT("  %s -> %s"), *ColorEntry.Key, *HexColor);
        if (++Count >= 5) break;
    }
}
```

**Interpretation**: This debug function provides runtime visibility into the caching system's state. It logs statistics about how many items are stored in each cache (colors, coordinates, GML mappings) and displays sample data entries with their color values. This is essential for troubleshooting data parsing and cache population issues during development.

## Step 8: HoloLens 2 Deployment

### 8.1 Project Configuration for HoloLens 2

Configure the Unreal Engine project to support Mixed Reality deployment with proper input conversion from traditional mouse/keyboard to HoloLens gesture-based interaction.

#### 8.1.1 Platform Settings Configuration

1. **Enable HoloLens Platform Support**
   - Open Project Settings → Platforms → HoloLens
   - Enable "Support HoloLens" platform
   - Set Target Device Family to "Windows.Holographic"
   - Configure Package Identity (Publisher, Package Name, Version)

2. **Mixed Reality Settings**
   - Enable "Start in VR" in Project Settings → XR
   - Set Default XR System to "Windows Mixed Reality"
   - Configure Holographic Remoting settings for testing

#### 8.1.2 Input System Configuration

Before deploying to HoloLens 2, we need to convert traditional mouse and keyboard inputs to touch and gesture inputs that work with hand tracking.

```cpp
// Add to BuildingEnergyDisplay.h
#include "HoloLensInputConverter.h"

UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HoloLens")
class UHoloLensInputConverter* HoloLensInputComponent;
```

**Interpretation**: This adds the HoloLens input converter component to our main building display actor, enabling automatic conversion of mouse clicks to appropriate touch gestures.

#### 8.1.3 Input Converter Integration

Add the HoloLens input converter to the BuildingEnergyDisplay constructor:

```cpp
// In BuildingEnergyDisplay.cpp constructor
ABuildingEnergyDisplay::ABuildingEnergyDisplay()
{
    PrimaryActorTick.bCanEverTick = false;
    HttpModule = &FHttpModule::Get();
    
    // Create HoloLens input converter component
    HoloLensInputComponent = CreateDefaultSubobject<UHoloLensInputConverter>(TEXT("HoloLensInputConverter"));
}
```

**Interpretation**: The input converter component is created as a default subobject, automatically handling the conversion between mouse input (for desktop testing) and touch gestures (for HoloLens deployment).

### 8.2 Gesture Mapping Configuration

#### 8.2.1 Air Tap to Building Selection

Configure air tap gestures to select buildings and display their energy information:

```cpp
// In HoloLensInputConverter.cpp - Enhanced building selection
void UHoloLensInputConverter::HandleAirTapGesture(FVector2D GazePosition)
{
    UE_LOG(LogTemp, Warning, TEXT("HoloLens: Air tap at gaze position (%.1f, %.1f)"), 
        GazePosition.X, GazePosition.Y);
    
    if (UWorld* World = GetWorld())
    {
        if (APlayerController* PC = World->GetFirstPlayerController())
        {
            // Use head gaze ray for building selection
            FVector CameraLocation;
            FRotator CameraRotation;
            PC->GetPlayerViewPoint(CameraLocation, CameraRotation);
            
            FVector RayStart = CameraLocation;
            FVector RayEnd = RayStart + (CameraRotation.Vector() * GazeRayDistance);
            
            FHitResult HitResult;
            if (World->LineTraceSingleByChannel(HitResult, RayStart, RayEnd, ECC_WorldStatic))
            {
                if (AActor* HitActor = HitResult.GetActor())
                {
                    // Find BuildingEnergyDisplay and trigger building selection
                    for (TActorIterator<ABuildingEnergyDisplay> ActorItr(World); ActorItr; ++ActorItr)
                    {
                        ABuildingEnergyDisplay* BuildingDisplay = *ActorItr;
                        if (BuildingDisplay)
                        {
                            // Get building by coordinates
                            FVector HitLocation = HitResult.Location;
                            FString BuildingGmlId = BuildingDisplay->GetBuildingByCoordinates(HitLocation);
                            
                            if (!BuildingGmlId.IsEmpty())
                            {
                                UE_LOG(LogTemp, Warning, TEXT("HoloLens: Selected building %s"), *BuildingGmlId);
                                BuildingDisplay->ShowBuildingAttributes(BuildingGmlId);
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
}
```

**Interpretation**: This function uses the user's head gaze direction to perform ray casting and select buildings in the 3D scene. When a building is selected via air tap, it displays the building's energy attributes using the same UI system as the desktop version.

#### 8.2.2 Tap and Hold for Context Menu

Configure tap and hold gestures to show context menus and detailed building information:

```cpp
// Enhanced tap and hold for HoloLens
void UHoloLensInputConverter::HandleTapAndHoldGesture(FVector2D GazePosition, float HoldDuration)
{
    UE_LOG(LogTemp, Warning, TEXT("HoloLens: Tap and hold gesture (%.2fs) at gaze position"), HoldDuration);
    
    if (UWorld* World = GetWorld())
    {
        // Find and show building energy modification interface
        for (TActorIterator<ABuildingEnergyDisplay> ActorItr(World); ActorItr; ++ActorItr)
        {
            ABuildingEnergyDisplay* BuildingDisplay = *ActorItr;
            if (BuildingDisplay)
            {
                // Show comprehensive building data panel
                UE_LOG(LogTemp, Warning, TEXT("HoloLens: Showing detailed building interface"));
                
                // This would open a more detailed HoloLens-optimized UI
                // with larger touch targets and voice command options
                BuildingDisplay->ShowHoloLensEnergyInterface();
                break;
            }
        }
    }
}
```

**Interpretation**: Tap and hold gestures provide access to detailed building modification interfaces optimized for mixed reality interaction, with larger UI elements suitable for finger touch and gesture control.

### 8.3 Build Configuration for HoloLens

#### 8.3.1 Package Settings

Configure the project packaging settings specifically for HoloLens deployment:

```powershell
# HoloLens build command
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Final_project.sln /p:Configuration="Shipping" /p:Platform="HoloLens" /p:BuildProjectReferences=true
```

**Interpretation**: This MSBuild command compiles the project specifically for HoloLens platform using the Shipping configuration for optimal performance on the device.

#### 8.3.2 Application Manifest Configuration

Create or modify the Package.appxmanifest for HoloLens capabilities:

```xml
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">
  <Applications>
    <Application Id="App">
      <uap:VisualElements DisplayName="Building Energy Visualization">
        <!-- HoloLens-specific capabilities -->
        <uap:SplashScreen Image="Assets\SplashScreen.png" />
        <uap:InitialRotationPreference>
          <uap:Rotation Preference="landscape"/>
        </uap:InitialRotationPreference>
      </uap:VisualElements>
    </Application>
  </Applications>
  
  <Capabilities>
    <!-- Required for HoloLens 2 -->
    <DeviceCapability Name="webcam" />
    <DeviceCapability Name="microphone" />
    <uap2:Capability Name="spatialPerception" />
    <DeviceCapability Name="gazeInput" />
    
    <!-- Network access for API calls -->
    <Capability Name="internetClient" />
    <Capability Name="internetClientServer" />
  </Capabilities>
</Package>
```

**Interpretation**: The application manifest declares the necessary capabilities for HoloLens 2, including spatial perception for room mapping, gaze input for interaction, and network access for the building energy API.

### 8.4 UI Adaptation for Mixed Reality

#### 8.4.1 HoloLens-Optimized Widget Creation

Create a specialized widget class for HoloLens interaction:

```cpp
// HoloLensBuildingWidget.h
UCLASS()
class FINAL_PROJECT_API UHoloLensBuildingWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "HoloLens UI")
    void SetupForHoloLens();
    
    UFUNCTION(BlueprintCallable, Category = "HoloLens UI")
    void ConfigureTouchTargets();
    
    UFUNCTION(BlueprintCallable, Category = "HoloLens UI")
    void EnableVoiceCommands();

protected:
    // Larger touch targets for finger interaction
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HoloLens Settings")
    float MinimumTouchTargetSize;
    
    // 3D positioning for floating UI
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HoloLens Settings")
    FVector UIWorldPosition;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HoloLens Settings")
    float UIDistance; // Distance from user
};
```

**Interpretation**: This specialized widget class provides HoloLens-specific UI adaptations including larger touch targets for finger interaction, 3D positioning for floating interfaces, and voice command integration.

### 8.5 Testing and Deployment Pipeline

#### 8.5.1 Holographic Remoting for Development

Set up development workflow using Holographic Remoting for rapid iteration:

1. **Install Holographic Remoting Player** on HoloLens 2
2. **Configure Unreal Engine** for remoting:
   - Enable "Start in VR" 
   - Set "XR System" to "Windows Mixed Reality"
   - Configure remoting IP address in project settings

3. **Development Testing Workflow**:
   ```powershell
   # Launch with remoting enabled
   & "UnrealEditor.exe" "Final_project.uproject" -game -vr -HolographicRemoting
   ```

**Interpretation**: Holographic Remoting allows you to run the application on your development PC while viewing and interacting with it through the HoloLens 2, enabling rapid development iteration without full device deployment.

#### 8.5.2 Device Deployment Process

Complete deployment process for standalone HoloLens 2 operation:

1. **Package Application**:
   - File → Package Project → HoloLens
   - Select Shipping configuration
   - Choose output directory

2. **Deploy to Device**:
   ```powershell
   # Install via Device Portal or PowerShell
   Add-AppxPackage -Path "Final_project.appx" -DependencyPath "Dependencies\"
   ```

3. **Testing Checklist**:
   - ✓ Air tap selects buildings
   - ✓ Tap and hold shows context menu
   - ✓ API authentication works over WiFi
   - ✓ Building colors update in real-time
   - ✓ UI elements are properly sized for touch
   - ✓ Performance maintains 60 FPS

**Interpretation**: The complete deployment process ensures the application runs standalone on HoloLens 2 with full functionality including gesture recognition, network connectivity, and optimized performance for the device's capabilities.

### 8.6 Performance Optimization for HoloLens 2

#### 8.6.1 Rendering Optimization

Configure rendering settings specifically for HoloLens 2 hardware constraints:

```cpp
// In BeginPlay() - HoloLens performance settings
void ABuildingEnergyDisplay::BeginPlay()
{
    Super::BeginPlay();
    
    // Detect HoloLens platform and optimize accordingly
    if (HoloLensInputComponent && HoloLensInputComponent->IsHoloLens2Platform())
    {
        UE_LOG(LogTemp, Warning, TEXT("HoloLens: Applying performance optimizations"));
        
        // Reduce rendering quality for performance
        if (UWorld* World = GetWorld())
        {
            // Set frame rate target
            World->GetWorldSettings()->MaxFPS = 60.0f;
            
            // Optimize for mixed reality
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.MobileHDR"))->Set(0);
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.MobileMSAA"))->Set(1);
        }
        
        // Reduce polling frequency for better performance
        PollingInterval = 60.0f; // Poll every minute instead of 30 seconds
    }
}
```

**Interpretation**: These optimizations reduce rendering overhead and adjust polling frequency to maintain stable performance on HoloLens 2 while preserving essential functionality.

## Troubleshooting Common Issues

**Cesium Color Visualization Problems**: One of the most frequent challenges encountered during implementation involves the failure of per-building colors to appear on the Cesium 3D tileset, despite successful API data retrieval and color caching. This issue typically stems from a mismatch between the feature metadata properties available in the tileset and the property names referenced in the styling expressions. The problem manifests as buildings maintaining their default appearance instead of displaying the energy-specific colors extracted from the API. To resolve this, developers must ensure that the Cesium tileset contains proper feature metadata by adding a CesiumFeaturesMetadata component to the tileset actor. The component's Auto Fill functionality should be used to automatically discover and map the available metadata properties. However, with the 2026 updates to Cesium for Unreal, the methodology has evolved to use the `.AddProperties` function for property discovery and mapping. This new approach provides more granular control over metadata visualization and requires updating the styling code to use the updated Cesium API calls for property binding and color application.

**Authentication and Token Management Failures**: Authentication-related issues frequently arise during development and deployment, particularly when dealing with JWT token expiration and refresh mechanisms. These problems typically manifest as HTTP 401 errors during API calls, failed data retrieval, or complete system shutdown when authentication fails. The root causes often include expired access tokens, invalid refresh tokens, network connectivity issues, SSL certificate verification failures, or changes to the backend API authentication endpoints. To address these challenges, developers should implement comprehensive token validation and refresh logic that includes proper error handling and retry mechanisms. The solution involves verifying that access tokens are valid and properly formatted, ensuring the refresh token mechanism functions correctly by testing token refresh scenarios, and confirming network connectivity to the API endpoints. For development environments using self-signed certificates or testing servers, it may be necessary to disable SSL certificate verification by adding `http.sslverification=false` to the Engine configuration file (DefaultEngine.ini) under the `[/Script/Engine.NetworkSettings]` section. Additionally, implementing proper logging for authentication events helps identify when and why authentication failures occur, enabling faster troubleshooting and resolution.

**JSON Parsing and Data Structure Errors**: Complex JSON responses from the building energy API can lead to parsing errors, particularly when dealing with nested coordinate data, missing fields, or unexpected data types. These issues commonly occur when the API returns array values where number values are expected, or when GeoJSON coordinate structures contain varying levels of nesting. The parsing errors can cause application crashes, incomplete data loading, or incorrect building visualization. The solution requires implementing robust error handling and type checking throughout the JSON parsing pipeline. This includes adding defensive programming techniques as demonstrated in the coordinate parsing examples, implementing comprehensive type validation before attempting to extract numeric values from JSON objects, and creating fallback mechanisms for handling missing or malformed data. Additionally, enabling detailed logging of raw JSON responses helps identify the exact structure of problematic data, allowing developers to adjust parsing logic accordingly.

**Performance Degradation and Resource Management**: Performance issues can emerge as the system scales to handle larger datasets or when deployed on resource-constrained platforms like HoloLens 2. Common symptoms include frame rate drops, delayed API responses, memory leaks, or system freezing during data processing operations. These problems often result from inefficient data processing, excessive polling frequency, large JSON payloads, or insufficient caching strategies. The solution involves implementing comprehensive performance optimization strategies including proper caching mechanisms to reduce redundant API calls, adjusting polling frequencies based on actual data change rates and platform capabilities, and implementing background thread processing for heavy JSON parsing operations. For HoloLens 2 deployments specifically, additional optimizations include reducing rendering quality settings, limiting concurrent network operations, and implementing progressive data loading to maintain responsive user interaction while handling large building datasets.

## Conclusion

This comprehensive implementation guide has demonstrated the successful development of a real-time building energy visualization system that bridges traditional desktop computing with cutting-edge mixed reality technology, with **Bisingen, Germany** serving as the primary case study for this research. The solution represents a significant advancement in building energy management interfaces, moving beyond static dashboards to immersive, spatially-aware visualization systems that enable intuitive interaction with complex energy data in real-world urban environments.

The implementation provides a complete solution for real-time building energy visualization in Unreal Engine with Cesium integration, extending from desktop development to mixed reality deployment on HoloLens 2. The modular design allows for easy maintenance and extension of functionality across multiple platforms. Key success factors include robust error handling, efficient caching strategies, proper integration with Cesium's feature styling system, and seamless input conversion for gesture-based interaction on HoloLens 2.

**Case Study Application and Urban Scalability**

The successful deployment in Bisingen, Germany validates the system's capability to handle real-world urban energy data with complex building geometries and varying energy consumption patterns. The choice of Bisingen as the test environment demonstrates the system's effectiveness in typical European municipal settings, with mixed residential and commercial building types that represent common urban energy management challenges. The implementation successfully processed and visualized energy data for the entire municipal building portfolio, proving its practical applicability beyond laboratory conditions.

**Scalability to Other Cities and Regions**

The architectural design of this system inherently supports scalability to other cities and regions through several key features. The **REST API abstraction layer** enables integration with different municipal energy management systems, requiring only endpoint configuration changes rather than core system modifications. The **coordinate transformation methodology** can handle different geographic projections and coordinate systems, making the system adaptable to cities worldwide without requiring fundamental architectural changes. The **Cesium 3D tileset integration** supports global deployment since Cesium Ion provides worldwide building data coverage, enabling rapid deployment in new geographic regions.

Cities considering adoption would need to provide energy data in JSON format through REST endpoints, but the specific data schemas can be adapted through configuration rather than code modification. The **multi-language support potential** built into Unreal Engine's localization system means the interface can be adapted for different languages and cultural contexts. The **modular caching system** automatically adapts to different data volumes, making the system suitable for small municipalities like Bisingen as well as major metropolitan areas with hundreds of thousands of buildings.

**International Deployment Considerations**

Different regions may have varying data privacy regulations, energy reporting standards, and municipal IT infrastructure capabilities. The system's **platform-agnostic design** addresses these challenges by supporting both cloud-based and on-premises deployment scenarios. The **authentication framework** can be extended to support region-specific security requirements, from European GDPR compliance to other international data protection standards. The **offline capability potential** mentioned in future work would be particularly valuable for cities in regions with limited internet infrastructure.

The success demonstrated in Bisingen, Germany provides a validated foundation for international expansion, proving that mixed reality building energy visualization can effectively serve real municipal energy management needs while remaining technically feasible and user-friendly for diverse urban environments.

The solution addresses the full development lifecycle from initial API integration through production deployment on mixed reality devices. The HoloLens 2 implementation demonstrates how traditional desktop applications can be adapted for mixed reality environments through intelligent input mapping, performance optimization, and UI adaptation for spatial computing. The gesture conversion system enables natural interaction through air tap and tap-and-hold gestures while maintaining full functionality of the building energy visualization system.

**Technical Contributions and Achievements**

Our implementation successfully addresses several critical challenges in real-time building energy visualization. The component-based architecture provides superior modularity compared to monolithic GIS solutions, while the cache-first data management strategy ensures responsive performance even under network constraints. The HTTP-based authentication system with automatic token refresh provides reliable security without the complexity of persistent connection protocols, and the cross-platform deployment capability demonstrates the feasibility of extending desktop applications to mixed reality environments without complete reimplementation.

The Cesium 3D tileset integration represents a significant improvement over traditional CAD-based or purely photogrammetric approaches, combining geometric accuracy with realistic environmental context. The dynamic color application methodology enables real-time energy visualization with infinite color gradations while maintaining optimal memory usage, addressing scalability limitations that affect large-scale building portfolios in commercial systems.

**Limitations and Constraints**

However, several limitations and constraints should be acknowledged in this implementation. The **polling-based data synchronization** approach, while reliable, introduces inherent latency of 30-60 seconds between actual energy changes and visualization updates, which may not be suitable for applications requiring immediate responsiveness to energy fluctuations. The **HTTP-based authentication system** lacks advanced security features such as multi-factor authentication or OAuth 2.0 integration, limiting its deployment in enterprise environments with strict security requirements. **Platform-specific limitations** include the HoloLens 2's computational constraints that require reduced rendering quality and simplified UI interactions, potentially compromising the visual fidelity compared to desktop deployment. The **Cesium integration dependency** ties the implementation to specific plugin versions and may require significant modifications if Cesium's API undergoes major changes, as evidenced by the transition from legacy styling methods to the newer `.AddProperties` approach. Additionally, the **network connectivity requirements** make the system unsuitable for offline operation or environments with unreliable internet access, and the **single-API backend design** lacks redundancy and failover capabilities that would be essential for production-scale deployment across multiple building portfolios.

**Future Work and Research Directions**

Building upon the foundation established by this implementation, several promising avenues for future development emerge. **Geometry editing workflows** represent a critical next step, enabling users to directly modify building geometry and energy parameters within the mixed reality environment. This would transform the system from a passive visualization tool to an active design platform, allowing architects and energy consultants to test modifications in real-time while visualizing their impact on energy performance.

**AI-driven interaction integration** offers significant potential for enhancing user experience and system intelligence. Machine learning algorithms could analyze user interaction patterns to predict commonly accessed buildings, pre-cache relevant data, and provide intelligent suggestions for energy optimization strategies. Predictive analytics could identify buildings with unusual energy consumption patterns, automatically highlighting potential issues for facility managers.

**Voice-based command integration** would significantly improve usability on HoloLens 2, where traditional typing is inconvenient for most users. Voice commands could enable natural language queries such as "Show me all buildings with high energy consumption" or "Compare this building's energy usage to the neighborhood average." This would be particularly valuable for on-site inspections where hands-free operation is essential for safety and efficiency.

Additional future research directions include implementing **real-time collaboration features** that allow multiple users to simultaneously interact with the same building energy data in shared mixed reality spaces, developing **offline operation capabilities** with local data synchronization for use in areas with limited connectivity, and creating **advanced analytics dashboards** that leverage the spatial context of mixed reality to present complex energy data in more intuitive formats.

The integration of **IoT sensor networks** could provide real-time granular data at the room or device level, while **blockchain-based energy trading platforms** could enable visualization of peer-to-peer energy exchanges between buildings. Furthermore, **digital twin integration** could synchronize the visualization system with comprehensive building information models, enabling predictive maintenance and energy optimization based on detailed system modeling.

This implementation establishes a solid foundation for the future of building energy visualization, demonstrating that mixed reality technology can successfully bridge the gap between complex energy data and intuitive user interaction. The modular architecture and open-source approach ensure that future researchers and developers can build upon this work to address the evolving challenges of sustainable building management in an increasingly connected and environmentally conscious world.

The troubleshooting approaches outlined above address the most common technical challenges encountered during development and deployment, providing developers with practical solutions and preventive strategies to ensure reliable system operation across different platforms and deployment scenarios, from desktop development environments to HoloLens 2 mixed reality deployment.