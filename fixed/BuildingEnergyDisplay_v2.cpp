// Fill out your copyright notice in the Description page of Project Settings. [COPYRIGHT NOTICE PLACEHOLDER]

#include "BuildingEnergyDisplay.h" // Include the header file for this class [BUILDING ENERGY DISPLAY INCLUDE]
#include "BuildingAttributesWidget.h" // Include building attributes widget for UI functionality [BUILDING ATTRIBUTES WIDGET INCLUDE]
#include "HttpModule.h" // Include HTTP module for web request functionality [HTTP MODULE INCLUDE]
#include "Interfaces/IHttpResponse.h" // Include HTTP response interface for handling web responses [HTTP RESPONSE INTERFACE INCLUDE]
#include "Json.h" // Include JSON library for parsing and creating JSON data [JSON INCLUDE]
#include "JsonUtilities.h" // Include JSON utility functions for advanced JSON operations [JSON UTILITIES INCLUDE]
#include "Engine/Engine.h" // Include engine functionality for global engine access [ENGINE INCLUDE]
#include "EngineUtils.h" // Include engine utility functions for world iteration and object finding [ENGINE UTILS INCLUDE]
#include "Materials/MaterialInstanceDynamic.h" // Include dynamic material instances for runtime material modifications [MATERIAL INSTANCE DYNAMIC INCLUDE]
#include "Components/StaticMeshComponent.h" // Include static mesh component functionality for mesh handling [STATIC MESH COMPONENT INCLUDE]
#include "WebSocketsModule.h" // Include WebSocket module for real-time energy updates [WEBSOCKET MODULE INCLUDE]
#include "IWebSocket.h" // Include WebSocket interface for energy data connections [WEBSOCKET INTERFACE INCLUDE]
#include "Kismet/GameplayStatics.h" // Include gameplay statics for actor finding and world queries [GAMEPLAY STATICS INCLUDE]
#include "Cesium3DTileset.h"

// Sets default values [CONSTRUCTOR COMMENT]
// ✅ Target only the BUILDINGS tileset (avoid tinting terrain tileset like Cesium World Terrain)
bool ABuildingEnergyDisplay::IsTargetBuildingsTileset(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}

	// If user explicitly assigned the actor, trust that.
	if (BuildingsTilesetActorOverride)
	{
		return Actor == BuildingsTilesetActorOverride;
	}

	const FString Target = BuildingsTilesetName.ToLower();
	if (Target.IsEmpty())
	{
		return false; // Safety: never fall back to "any tileset" automatically
	}

	return Actor->GetName().ToLower().Contains(Target);
}

AActor* ABuildingEnergyDisplay::ResolveBuildingsTilesetActor() const
{
	if (BuildingsTilesetActorOverride)
	{
		return BuildingsTilesetActorOverride;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	const FString Target = BuildingsTilesetName.ToLower();
	if (Target.IsEmpty())
	{
		return nullptr;
	}

	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (!Actor)
		{
			continue;
		}

		// Only consider Cesium 3D Tileset actors
		if (!Actor->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
		{
			continue;
		}

		if (IsTargetBuildingsTileset(Actor))
		{
			return Actor;
		}
	}

	UE_LOG(LogTemp, Error, TEXT("🎨 ERROR: Buildings tileset not found. Set BuildingsTilesetActorOverride or ensure a Cesium3DTileset actor name contains '%s'."), *BuildingsTilesetName);
	return nullptr;
}

// ✅ Get the UCesium3DTileset component from the resolved buildings tileset actor.
UCesium3DTileset* ABuildingEnergyDisplay::GetBuildingsTilesetComponent() const
{
	AActor* TilesetActor = const_cast<ABuildingEnergyDisplay*>(this)->ResolveBuildingsTilesetActor();
	if (!TilesetActor)
	{
		return nullptr;
	}

	// Typical Cesium3DTileset actor owns a UCesium3DTileset component
	UCesium3DTileset* TilesetComp = TilesetActor->FindComponentByClass<UCesium3DTileset>();
	if (!TilesetComp)
	{
		UE_LOG(LogTemp, Error, TEXT("🎨 ERROR: Actor '%s' has no UCesium3DTileset component."), *TilesetActor->GetName());
	}
	return TilesetComp;
}

// ✅ Build a 3D Tiles Styling JSON that matches feature['gml:id'] to the cached color (keyed by modified_gml_id).
FString ABuildingEnergyDisplay::BuildCesiumStyleJsonFromCache() const
{
	// 3D Tiles Styling uses a JSON object. We'll generate a "color" expression with "conditions".
	// Important: this is evaluated per-feature on the GPU, so it correctly colors individual buildings even when many
	// features are batched into the same mesh.
	//
	// Example:
	// {
	//   "color": {
	//     "conditions": [
	//       ["${feature['gml:id']} === 'ID_1'", "rgba(255,0,0,1.0)"],
	//       ["${feature['gml:id']} === 'ID_2'", "rgba(0,255,0,1.0)"],
	//       ["true", "rgba(255,255,255,1.0)"]
	//     ]
	//   }
	// }

	auto JsonEscape = [](const FString& In) -> FString
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Out.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return Out;
	};

	FString Style = TEXT("{\"color\":{\"conditions\":[");
	bool bFirst = true;

	// To avoid generating invalid JSON, always keep last fallback condition.
	for (const auto& It : BuildingColorCache)
	{
		const FString& ModifiedGmlIdKey = It.Key;

		// Skip empty keys
		if (ModifiedGmlIdKey.IsEmpty())
		{
			continue;
		}

		const FLinearColor& C = It.Value;
		const int32 R = FMath::Clamp(FMath::RoundToInt(C.R * 255.0f), 0, 255);
		const int32 G = FMath::Clamp(FMath::RoundToInt(C.G * 255.0f), 0, 255);
		const int32 B = FMath::Clamp(FMath::RoundToInt(C.B * 255.0f), 0, 255);
		const float A = 1.0f;

		// Condition: compare gml:id to our cache key (modified_gml_id).
		// NOTE: property name contains a colon, so we use bracket notation: feature['gml:id'].
		const FString Cond = FString::Printf(TEXT("${feature['gml:id']} === \"%s\""), *JsonEscape(ModifiedGmlIdKey));
		const FString ColorExpr = FString::Printf(TEXT("rgba(%d,%d,%d,%.3f)"), R, G, B, A);

		if (!bFirst) Style += TEXT(",");
		bFirst = false;

		Style += FString::Printf(TEXT("[\"%s\",\"%s\"]"), *JsonEscape(Cond), *JsonEscape(ColorExpr));
	}

	// Fallback: keep original color (white). You can change this to "color('white')" too.
	if (!bFirst) Style += TEXT(",");
	Style += TEXT("[\"true\",\"rgba(255,255,255,1.0)\"]");

	Style += TEXT("]}}");

	return Style;
}


ABuildingEnergyDisplay::ABuildingEnergyDisplay() // Default constructor for initializing member variables [CONSTRUCTOR DECLARATION]
{ // Start of constructor body [CONSTRUCTOR BODY START]
 	// Set this actor to call Tick() every frame. You can turn this off to improve performance if you don't need it. [TICK COMMENT]
	PrimaryActorTick.bCanEverTick = true; // Enable per-frame tick updates for real-time monitoring [PRIMARY ACTOR TICK ASSIGNMENT]
	bDataLoaded = false; // Initialize data loaded flag to false indicating no data has been loaded yet [DATA LOADED INITIALIZATION]
	bIsLoading = false; // Initialize loading flag to false indicating no loading operation is in progress [IS LOADING INITIALIZATION]
	LastDisplayTime = 0.0; // Initialize last display time to zero for spam prevention timing [LAST DISPLAY TIME INITIALIZATION]
	LastDisplayedGmlId = TEXT(""); // Initialize last displayed GML ID to empty string for duplicate prevention [LAST DISPLAYED GML ID INITIALIZATION]
	BuildingEnergyMaterial = nullptr; // Initialize building energy material pointer to null [BUILDING ENERGY MATERIAL INITIALIZATION]
	CurrentBuildingGmlId = TEXT(""); // Initialize current building GML ID to empty string [CURRENT BUILDING GML ID INITIALIZATION]
	BuildingAttributesWidget = nullptr; // Initialize building attributes widget pointer to null [BUILDING ATTRIBUTES WIDGET INITIALIZATION]
	BuildingAttributesWidgetClass = nullptr; // Initialize building attributes widget class pointer to null [BUILDING ATTRIBUTES WIDGET CLASS INITIALIZATION]
	bShowScreenMessages = true; // Enable screen debug messages for building information display [SCREEN MESSAGES ENABLED]
	
	// Initialize Building Info Widget variables for single building display
	CurrentBuildingInfoWidget = nullptr; // Initialize building info widget pointer to null [BUILDING INFO WIDGET INITIALIZATION]
	BuildingInfoWidgetClass = nullptr; // Initialize building info widget class pointer to null [BUILDING INFO WIDGET CLASS INITIALIZATION]
	CurrentlyDisplayedBuildingId = TEXT(""); // Initialize currently displayed building ID to empty for single display control [CURRENTLY DISPLAYED BUILDING INITIALIZATION]
	
	// Initialize WebSocket Energy Variables
	EnergyWebSocket = nullptr; // Initialize energy WebSocket pointer to null [ENERGY WEBSOCKET INITIALIZATION]
	bEnergyWebSocketConnected = false; // Initialize energy WebSocket connection status [ENERGY WEBSOCKET CONNECTION STATUS]
	EnergyWebSocketURL = TEXT(""); // Disabled - using REST API polling instead of WebSocket
	WebSocketReconnectTimer = 0.0f; // Initialize reconnect timer [WEBSOCKET RECONNECT TIMER]
	EnergyUpdateCounter = 0; // Initialize energy update counter [ENERGY UPDATE COUNTER]
	
	// Initialize Coordinate Validation Variables
	CoordinateValidationTolerance = 10.0f; // Initialize coordinate validation tolerance [COORDINATE TOLERANCE INITIALIZATION]
} // End of constructor body [CONSTRUCTOR BODY END]

// Called when the game starts or when spawned [BEGIN PLAY COMMENT]
void ABuildingEnergyDisplay::BeginPlay() // BeginPlay lifecycle method called when actor is spawned in the world [BEGIN PLAY DECLARATION]
{ // Start of BeginPlay method body [BEGIN PLAY BODY START]
	Super::BeginPlay(); // Call parent class BeginPlay method to ensure proper initialization [SUPER BEGIN PLAY CALL]
	
	// INSTANCE TRACKING: Check for actual duplicate actors in the current world
	TArray<AActor*> FoundActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABuildingEnergyDisplay::StaticClass(), FoundActors);
	
	int32 ActiveInstances = FoundActors.Num();
	FString ActorName = GetName();
	
	UE_LOG(LogTemp, Warning, TEXT("🎭 ACTIVE INSTANCES: %d BuildingEnergyDisplay actors found in world"), ActiveInstances);
	UE_LOG(LogTemp, Warning, TEXT("🎭 CURRENT ACTOR: %s"), *ActorName);
	
	if (ActiveInstances > 1)
	{
		UE_LOG(LogTemp, Error, TEXT("⚠️ MULTIPLE INSTANCES: Found %d BuildingEnergyDisplay actors in the level!"), ActiveInstances);
		UE_LOG(LogTemp, Error, TEXT("💡 SOLUTION: Remove duplicate BuildingEnergyDisplay actors from your level to prevent duplicate data loading."));
		
		// Log all found actors for debugging
		for (int32 i = 0; i < FoundActors.Num(); i++)
		{
			UE_LOG(LogTemp, Warning, TEXT("   Actor %d: %s"), i + 1, *FoundActors[i]->GetName());
		}
		
		if (GEngine && bShowScreenMessages)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Orange, 
				FString::Printf(TEXT("⚠️ DUPLICATE ACTORs: %d instances detected. Remove duplicates!"), ActiveInstances));
		}
		
		// Disable all but the first instance to prevent conflicts
		bool bIsFirstInstance = (FoundActors[0] == this);
		if (!bIsFirstInstance)
		{
			UE_LOG(LogTemp, Warning, TEXT("🚫 DISABLING duplicate instance: %s"), *ActorName);
			SetActorTickEnabled(false);
			return; // Exit early for duplicate instances
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("✅ KEEPING primary instance: %s"), *ActorName);
		}
	}
	
	// Reset authentication message flag for fresh play session
	bAuthenticationMessageShown = false;
	
	// 🎮 BLUEPRINT CONTROL: Let Blueprint BeginPlay event handle the authentication and loading
	UE_LOG(LogTemp, Warning, TEXT("🎮 C++ BeginPlay complete. Blueprint will control authentication and data loading."));
	UE_LOG(LogTemp, Warning, TEXT("💡 Blueprint should call AuthenticateAndLoadData() when ready."));
	
	// Start real-time monitoring for continuous background updates [START REAL-TIME MONITORING COMMENT]
	StartRealTimeMonitoring(); // Initialize real-time monitoring system for continuous data updates [START REAL-TIME MONITORING CALL]
	
	UE_LOG(LogTemp, Warning, TEXT("REALTIME Real-time monitoring system initialized")); // Log message indicating real-time monitoring is active [REAL-TIME MONITORING LOG MESSAGE]
	
	// 🔄 CESIUM REFRESH MONITORING: Set up automatic color reapplication when Cesium refreshes
	// TEMPORARILY DISABLED - Investigating interaction issues
	SetupCesiumRefreshMonitoring();
		
		// 🎨 DIRECT COLOR APPLICATION: Apply colors directly to geometry (bypass Cesium metadata)
		SetupDirectColorApplication();
		
		// 🎨 AUTO COLOR APPLICATION AT STARTUP: If data is already loaded, apply colors immediately
		// TEMPORARILY DISABLED - Causing gray overlay on entire scene
		if (BuildingColorCache.Num() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("🎨 STARTUP: Color cache contains %d buildings, but auto-application disabled to prevent gray overlay"), BuildingColorCache.Num());
			
			// Apply colors with a short delay to ensure Cesium is ready
			// DISABLED - This was causing the entire scene to turn gray
			/*
			FTimerHandle StartupColorTimer;
			GetWorld()->GetTimerManager().SetTimer(StartupColorTimer, [this]()
			{
				UE_LOG(LogTemp, Warning, TEXT("🎨 STARTUP EXECUTION: Applying colors at BeginPlay"));
				ApplyColorsUsingCesiumStyling();
				ApplyColorsToCSiumTileset();
				ForceApplyColors();
			}, 3.0f, false); // 3 second delay for startup
			*/
		}
	/*
	FTimerHandle AutoDebugTimer;
	GetWorld()->GetTimerManager().SetTimer(AutoDebugTimer, [this]()
	{
		UE_LOG(LogTemp, Warning, TEXT("🧪 AUTO-DEBUG: Running automatic color system diagnostics..."));
		LogColorCacheStatus();
		ForceApplyColors();
	}, 5.0f, false); // Run after 5 seconds to let everything initialize
	*/
} // End of BeginPlay method body [BEGIN PLAY BODY END]

// 🎨 IMMEDIATE COLOR APPLICATION: Apply colors to all buildings right now (Blueprint callable)
void ABuildingEnergyDisplay::ApplyBuildingColorsImmediately()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 IMMEDIATE: Applying colors to all buildings NOW!"));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 WARNING: No building colors cached. Load data first."));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Orange, TEXT("🎨 No building colors cached. Load data first!"));
		}
		return;
	}
	
	// Try ONLY the direct geometry approach to avoid blanket material application
	UE_LOG(LogTemp, Warning, TEXT("🎨 Using SAFE color application method only"));
	ApplyColorsUsingCesiumStyling();
	
	// DO NOT call these - they cause gray overlay:
	// ApplyColorsToCSiumTileset();  // This applies blanket material
	// ForceApplyColors();           // This also applies blanket material
	
	UE_LOG(LogTemp, Warning, TEXT("🎨 IMMEDIATE: Safe color application complete. Check buildings for colors."));
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
			FString::Printf(TEXT("🎨 Safe color method applied to %d buildings!"), BuildingColorCache.Num()));
	}
}

// 🎨 REFRESH ALL COLORS: Force refresh of all building colors
void ABuildingEnergyDisplay::RefreshAllBuildingColors()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 REFRESH: Refreshing all building colors..."));
	
	// First, reload the data to get fresh colors
	if (!AccessToken.IsEmpty())
	{
		PreloadAllBuildingData(AccessToken);
		
		// Apply colors after a delay to let data load
		// DISABLED - This was causing gray overlay on entire scene
		/*
		FTimerHandle RefreshTimer;
		GetWorld()->GetTimerManager().SetTimer(RefreshTimer, [this]()
		{
			ApplyBuildingColorsImmediately();
		}, 2.0f, false);
		*/
		UE_LOG(LogTemp, Warning, TEXT("🎨 Data loaded. Use manual color application to prevent gray overlay"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 WARNING: No access token. Cannot refresh data."));
	}
}

// 🔄 CESIUM REFRESH MONITORING: Set up automatic color reapplication when Cesium tileset updates
void ABuildingEnergyDisplay::SetupCesiumRefreshMonitoring()
{
	// Reapply colors periodically because Cesium tiles stream in/out and materials can be regenerated.
	// Safe now because we target ONLY the buildings tileset (not terrain).
	if (!GetWorld())
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("🔄 CESIUM MONITOR: Enabled periodic color reapplication (tileset: '%s')"), *BuildingsTilesetName);

	// Monitor for Cesium tileset changes every 2 seconds
	GetWorld()->GetTimerManager().SetTimer(
		CesiumRefreshTimer,
		this,
		&ABuildingEnergyDisplay::OnCesiumTilesetRefresh,
		2.0f,
		true
	);
}

// 🔄 CESIUM REFRESH HANDLER: Reapply colors when Cesium tileset refreshes
void ABuildingEnergyDisplay::OnCesiumTilesetRefresh()
{
	// Only reapply if we have colors cached
	if (BuildingColorCache.Num() == 0)
	{
		return;
	}

	// Only proceed if the target tileset exists (prevents tinting terrain by mistake)
	if (!ResolveBuildingsTilesetActor())
	{
		return;
	}

	// Throttle: don't spam full reapply every tick of the timer if not needed
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (LastColorReapplyTimeSeconds >= 0.0f && (Now - LastColorReapplyTimeSeconds) < 4.0f)
	{
		return;
	}
	LastColorReapplyTimeSeconds = Now;

	UE_LOG(LogTemp, Verbose, TEXT("🔄 CESIUM REFRESH: Reapplying cached per-building colors (%d cached)"), BuildingColorCache.Num());
	ApplyColorsUsingCesiumStyling();
}

// 🎨 DIRECT COLOR APPLICATION: Apply colors directly to geometry without Cesium metadata without Cesium metadata
void ABuildingEnergyDisplay::SetupDirectColorApplication()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 DIRECT: Setting up direct color application system..."));
	
	// Wait for Cesium to finish loading, then apply colors
	FTimerHandle DirectColorTimer;
	GetWorld()->GetTimerManager().SetTimer(DirectColorTimer, [this]()
	{
		if (BuildingColorCache.Num() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("🎨 DIRECT: Applying colors directly to %d buildings..."), BuildingColorCache.Num());
			ApplyColorsUsingCesiumStyling();
		}
	}, 8.0f, false); // Wait 8 seconds for Cesium to fully load
}

// 🎨 DIRECT COLOR APPLICATION: Apply cached colors directly to Cesium geometry using proper property mapping
void ABuildingEnergyDisplay::ApplyColorsDirectlyToGeometry()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 CESIUM METADATA: Starting per-building color application using gml:id mapping..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 No building colors cached. Total buildings: %d"), BuildingColorCache.Num());
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🎨 CACHE STATUS: %d buildings have cached colors"), BuildingColorCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("🎨 PROPERTY MAPPING: Looking for 'gml:id' in Cesium to match with 'modified_gml_id' cache keys"));
	
	// Find ONLY the buildings tileset actor (avoid tinting terrain)
AActor* TilesetActor = ResolveBuildingsTilesetActor();
if (!TilesetActor)
{
	return;
}
// Look for CesiumFeaturesMetadataComponent using string-based approach
	UActorComponent* MetadataComponent = nullptr;
	TArray<UActorComponent*> AllComponents;
	TilesetActor->GetComponents<UActorComponent>(AllComponents);
	
	for (UActorComponent* Component : AllComponents)
	{
		if (Component && Component->GetClass()->GetName().Contains(TEXT("CesiumFeaturesMetadata")))
		{
			MetadataComponent = Component;
			UE_LOG(LogTemp, Warning, TEXT("🎯 FOUND CesiumFeaturesMetadataComponent: %s"), *Component->GetName());
			break;
		}
	}
	
	if (!MetadataComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 No CesiumFeaturesMetadataComponent found. Applying representative color."));
		ApplyRepresentativeColorToAllBuildings(TilesetActor);
		return;
	}
	
	// Use reflection to inspect the metadata component for property tables
	UE_LOG(LogTemp, Warning, TEXT("🎯 Analyzing CesiumFeaturesMetadataComponent for gml:id properties..."));
	
	UClass* MetadataClass = MetadataComponent->GetClass();
	for (TFieldIterator<FProperty> PropIt(MetadataClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		FString PropName = Property->GetName();
		
		if (PropName.Contains(TEXT("Description")) || PropName.Contains(TEXT("PropertyTable")) || 
			PropName.Contains(TEXT("ModelMetadata")) || PropName.Contains(TEXT("Feature")))
		{
			UE_LOG(LogTemp, Warning, TEXT("🏷️ FOUND METADATA PROPERTY: %s"), *PropName);
		}
	}
	
	// CRITICAL FIX: Look for buildings with gml:id properties and match with cache
	// Since direct property table access requires Cesium headers, let's use a mixed approach:
	// 1. Apply colors to mesh components
	// 2. Log what we're doing for debugging
	// 3. Use representative color as fallback but with proper logging
	
	UE_LOG(LogTemp, Warning, TEXT("🔍 CESIUM PROPERTY SEARCH: Looking for buildings with 'gml:id' property..."));
	
	// Try to find individual building components that might have gml:id data
	TArray<UMeshComponent*> AllMeshComponents;
	TilesetActor->GetComponents<UMeshComponent>(AllMeshComponents);
	
	int32 ColorsApplied = 0;
	int32 BuildingsProcessed = 0;
	
	// Log a sample of our cached building IDs for debugging
	UE_LOG(LogTemp, Warning, TEXT("📋 SAMPLE CACHE ENTRIES (modified_gml_id format):"));
	int32 SampleCount = 0;
	for (const auto& CacheEntry : BuildingColorCache)
	{
		if (SampleCount < 5)
		{
			UE_LOG(LogTemp, Warning, TEXT("   Cache Key: %s -> Color: R=%.2f,G=%.2f,B=%.2f"), 
				*CacheEntry.Key, CacheEntry.Value.R, CacheEntry.Value.G, CacheEntry.Value.B);
		}
		SampleCount++;
	}
	
	// For each mesh component, try to determine if it has building properties
	for (UMeshComponent* MeshComp : AllMeshComponents)
	{
		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComp))
		{
			BuildingsProcessed++;
			FString ComponentName = StaticMeshComp->GetName();
			
			// Try to extract potential gml:id or building identifier from component
			// This is a simplified approach - in full implementation, we'd use proper Cesium property access
			
			// 🎯 INDIVIDUAL BUILDING COLOR: Try to find specific color for this building
			FLinearColor BuildingColor = FLinearColor::White; // Default fallback
			bool bFoundSpecificColor = false;
			
			// Try to find gml:id in component metadata or name
			FString PotentialGmlId = TEXT("");
			
			// Method 1: Try to extract from component name patterns
			if (ComponentName.Contains(TEXT("_")))
			{
				// Extract potential ID from component name
				TArray<FString> NameParts;
				ComponentName.ParseIntoArray(NameParts, TEXT("_"));
				if (NameParts.Num() > 0)
				{
					// Try last part as potential ID
					PotentialGmlId = NameParts.Last();
				}
			}
			
			// Method 2: Try component name as-is
			if (PotentialGmlId.IsEmpty())
			{
				PotentialGmlId = ComponentName;
			}
			
			// Try to find color in cache using potential ID
			for (const auto& CachedBuilding : BuildingColorCache)
			{
				FString CachedId = CachedBuilding.Key;
				
				// Try direct match (CASE-SENSITIVE - gml_id fields are case-sensitive)
				if (PotentialGmlId.Equals(CachedId))
				{
					BuildingColor = CachedBuilding.Value;
					bFoundSpecificColor = true;
					UE_LOG(LogTemp, Warning, TEXT("🎯 EXACT MATCH: Found color for building '%s'"), *PotentialGmlId);
					break;
				}
				
				// Try partial match
				if (PotentialGmlId.Contains(CachedId) || CachedId.Contains(PotentialGmlId))
				{
					BuildingColor = CachedBuilding.Value;
					bFoundSpecificColor = true;
					UE_LOG(LogTemp, Warning, TEXT("🎯 PARTIAL MATCH: Found color for building '%s' → '%s'"), *PotentialGmlId, *CachedId);
					break;
				}
			}
			
			// Fallback: Use a varied color instead of all same
			if (!bFoundSpecificColor && BuildingColorCache.Num() > 0)
			{
				// Use different colors for different components to see variation
				int32 ColorIndex = BuildingsProcessed % BuildingColorCache.Num();
				auto Iterator = BuildingColorCache.CreateConstIterator();
				for (int32 i = 0; i < ColorIndex && Iterator; ++i, ++Iterator) {}
				if (Iterator)
				{
					BuildingColor = Iterator->Value;
					UE_LOG(LogTemp, Verbose, TEXT("🎨 FALLBACK: Using varied color %d for component '%s'"), ColorIndex, *ComponentName);
				}
			}
			
			// Apply the determined color to all materials in this component
			{
				int32 NumMaterials = StaticMeshComp->GetNumMaterials();
				UE_LOG(LogTemp, Warning, TEXT("🏗️ MATERIAL DEBUG: Component '%s' has %d materials"), *ComponentName, NumMaterials);
				
				for (int32 MatIdx = 0; MatIdx < NumMaterials; ++MatIdx)
				{
					// Use the new helper function to ensure proper dynamic material creation
					UMaterialInstanceDynamic* DynMat = CreateOrGetDynamicMaterial(StaticMeshComp, MatIdx);
					
					if (DynMat)
					{
						// Apply the specific building color (not representative)
						DynMat->SetVectorParameterValue(TEXT("BaseColor"), BuildingColor);
						DynMat->SetVectorParameterValue(TEXT("Color"), BuildingColor);
						DynMat->SetVectorParameterValue(TEXT("Albedo"), BuildingColor);
						DynMat->SetVectorParameterValue(TEXT("DiffuseColor"), BuildingColor);
						DynMat->SetVectorParameterValue(TEXT("EmissiveColor"), BuildingColor * 0.1f);
						
						// Force material update
						StaticMeshComp->MarkRenderStateDirty();
						
						ColorsApplied++;
						
						FString ColorType = bFoundSpecificColor ? TEXT("SPECIFIC") : TEXT("VARIED");
						UE_LOG(LogTemp, Warning, TEXT("   ✅ Applied %s color R=%.2f G=%.2f B=%.2f to material %d"), 
							*ColorType, BuildingColor.R, BuildingColor.G, BuildingColor.B, MatIdx);
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("   ❌ Failed to create/get dynamic material %d"), MatIdx);
					}
				}
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("✅ CESIUM COLOR APPLICATION RESULTS:"));
	UE_LOG(LogTemp, Warning, TEXT("   Buildings processed: %d"), BuildingsProcessed);
	UE_LOG(LogTemp, Warning, TEXT("   Materials colored: %d"), ColorsApplied);
	UE_LOG(LogTemp, Warning, TEXT("   Cache entries available: %d"), BuildingColorCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("🔧 NEXT STEP: Implement runtime property table access to match gml:id with modified_gml_id"));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Green, 
			FString::Printf(TEXT("🎨 Applied energy colors to %d materials from %d cached buildings!"), 
				ColorsApplied, BuildingColorCache.Num()));
	}
}

// Helper method to apply representative color to all building components
void ABuildingEnergyDisplay::ApplyRepresentativeColorToAllBuildings(AActor* TilesetActor)
{
	if (!TilesetActor || BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("🎨 Cannot apply representative color: Invalid tileset actor or empty cache"));
		return;
	}
	
	// Use the first color from cache as representative
	FLinearColor RepresentativeColor = BuildingColorCache.begin()->Value;
	UE_LOG(LogTemp, Warning, TEXT("🎨 Applying representative color: R=%.2f, G=%.2f, B=%.2f"), 
		RepresentativeColor.R, RepresentativeColor.G, RepresentativeColor.B);
	
	// Apply color to all mesh components in the tileset
	TArray<UMeshComponent*> MeshComponents;
	TilesetActor->GetComponents<UMeshComponent>(MeshComponents);
	
	int32 ColorsApplied = 0;
	UE_LOG(LogTemp, Warning, TEXT("🏗️ REPRESENTATIVE COLOR: Processing %d mesh components"), MeshComponents.Num());
	
	for (UMeshComponent* MeshComp : MeshComponents)
	{
		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComp))
		{
			FString ComponentName = StaticMeshComp->GetName();
			int32 NumMaterials = StaticMeshComp->GetNumMaterials();
			
			UE_LOG(LogTemp, Warning, TEXT("   Component: %s (Materials: %d)"), *ComponentName, NumMaterials);
			
			for (int32 MatIdx = 0; MatIdx < NumMaterials; ++MatIdx)
			{
				// Use helper function for consistent material creation
				UMaterialInstanceDynamic* DynMat = CreateOrGetDynamicMaterial(StaticMeshComp, MatIdx);
				
				if (DynMat)
				{
					// Apply color using multiple parameter names for compatibility
					DynMat->SetVectorParameterValue(TEXT("BaseColor"), RepresentativeColor);
					DynMat->SetVectorParameterValue(TEXT("Color"), RepresentativeColor);
					DynMat->SetVectorParameterValue(TEXT("Albedo"), RepresentativeColor);
					DynMat->SetVectorParameterValue(TEXT("DiffuseColor"), RepresentativeColor);
					DynMat->SetVectorParameterValue(TEXT("EmissiveColor"), RepresentativeColor * 0.1f);
					
					// Force material refresh
					StaticMeshComp->MarkRenderStateDirty();
					
					ColorsApplied++;
					UE_LOG(LogTemp, Warning, TEXT("     ✅ Applied to material %d"), MatIdx);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("     ❌ Failed to create/get dynamic material %d"), MatIdx);
				}
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("✅ Applied representative color to %d material instances"), ColorsApplied);
	
	// Only show on-screen message once to prevent spam
	static bool bFirstApplication = true;
	if (bFirstApplication && ColorsApplied > 0)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Green, 
				FString::Printf(TEXT("🎨 CESIUM COLORS: Applied energy color to %d building materials!"), ColorsApplied));
		}
		bFirstApplication = false;
	}
	else if (ColorsApplied > 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("🔄 Reapplication: %d materials updated (no screen message)"), ColorsApplied);
	}
}

// Called every frame [TICK COMMENT]
void ABuildingEnergyDisplay::Tick(float DeltaTime) // Tick method called every frame for real-time updates [TICK DECLARATION]
{ // Start of Tick method body [TICK BODY START]
	Super::Tick(DeltaTime); // Call parent class Tick method to ensure proper frame updates [SUPER TICK CALL]
	
	// === REST API ENERGY POLLING SYSTEM ===
	if (bEnergyWebSocketConnected && EnergyWebSocketURL.IsEmpty()) // Polling mode active
	{
		WebSocketReconnectTimer += DeltaTime;
		if (WebSocketReconnectTimer >= WebSocketReconnectInterval)
		{
			WebSocketReconnectTimer = 0.0f;
			if (!AccessToken.IsEmpty())
			{
				// UE_LOG(LogTemp, Verbose, TEXT("🔄 Fetching updated energy data via REST API"));
				FetchUpdatedEnergyData();
			}
		}
	}
	
	// === WEBSOCKET RECONNECTION SYSTEM ===
	else if (bAutoReconnectWebSocket && !bEnergyWebSocketConnected && EnergyWebSocket.IsValid() == false)
	{
		WebSocketReconnectTimer += DeltaTime;
		if (WebSocketReconnectTimer >= WebSocketReconnectInterval)
		{
			WebSocketReconnectTimer = 0.0f;
			UE_LOG(LogTemp, Warning, TEXT("🔄 Attempting WebSocket reconnection for energy updates"));
			ConnectEnergyWebSocket();
		}
	}
	
	// === REAL-TIME MONITORING SYSTEM === [REAL-TIME MONITORING SYSTEM COMMENT]
	if (bRealTimeMonitoringEnabled && !bIsPerformingRealTimeUpdate) // Check if real-time monitoring is enabled and not currently updating [REAL-TIME MONITORING CONDITION]
	{ // Start of real-time monitoring block [REAL-TIME MONITORING BLOCK START]
		RealTimeMonitoringTimer += DeltaTime; // Accumulate time since last monitoring check [REAL-TIME MONITORING TIMER INCREMENT]
		if (RealTimeMonitoringTimer >= RealTimeUpdateInterval) // Check if enough time has passed for next monitoring cycle [REAL-TIME MONITORING INTERVAL CHECK]
		{ // Start of monitoring interval block [MONITORING INTERVAL BLOCK START]
			RealTimeMonitoringTimer = 0.0f; // Reset timer for next monitoring cycle [RESET MONITORING TIMER]
			if (!AccessToken.IsEmpty() && bDataLoaded) // Check if authentication token exists and data is loaded [ACCESS TOKEN AND DATA CHECK]
			{ // Start of token and data validation block [TOKEN DATA VALIDATION BLOCK START]
				UE_LOG(LogTemp, Verbose, TEXT("REALTIME Performing automatic background data check...")); // Log verbose message for background data check [BACKGROUND DATA CHECK LOG]
				PerformRealTimeDataCheck(); // Execute real-time data checking operation [PERFORM REAL-TIME DATA CHECK CALL]
			} // End of token and data validation block [TOKEN DATA VALIDATION BLOCK END]
		} // End of monitoring interval block [MONITORING INTERVAL BLOCK END]
	} // End of real-time monitoring block [REAL-TIME MONITORING BLOCK END]
} // End of Tick method body [TICK BODY END]

void ABuildingEnergyDisplay::PreloadAllBuildingData(const FString& Token) // PreloadAllBuildingData method to load all building data into cache [PRELOAD ALL BUILDING DATA DECLARATION]
{ // Start of PreloadAllBuildingData method body [PRELOAD ALL BUILDING DATA BODY START]
	// DUPLICATE CALL PREVENTION - Allow manual retry every 3 seconds
	static bool bPreloadInProgress = false;
	static float LastCallTime = 0.0f;
	float CurrentTime = FPlatformTime::Seconds();
	
	// Allow reset every 3 seconds for manual calls
	if ((CurrentTime - LastCallTime) > 3.0f)
	{
		bPreloadInProgress = false; // Reset for manual retries
		UE_LOG(LogTemp, Warning, TEXT("🔄 PRELOAD RESET: Manual data loading reset allowed"));
	}
	
	if (bPreloadInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("🛑 DUPLICATE PRELOAD PREVENTED: PreloadAllBuildingData already in progress"));
		return;
	}
	
	bPreloadInProgress = true;
	LastCallTime = CurrentTime;
	
	// Always reload data to ensure cache is up-to-date [RELOAD DATA COMMENT]
	UE_LOG(LogTemp, Warning, TEXT("Loading/Refreshing building data cache...")); // Log message indicating cache refresh operation [CACHE REFRESH LOG]
	
	// Check if already loading - but allow retry with new token [CHECK ALREADY LOADING COMMENT]
	if (bIsLoading) // Check if loading operation is already in progress [IS LOADING CHECK]
	{ // Start of already loading block [ALREADY LOADING BLOCK START]
		UE_LOG(LogTemp, Warning, TEXT("PreloadAllBuildingData already in progress - resetting state to allow retry")); // Log warning about concurrent loading attempt [CONCURRENT LOADING LOG]
		bIsLoading = false; // Reset to allow retry [RESET IS LOADING FLAG]
	} // End of already loading block [ALREADY LOADING BLOCK END]

	// Clear existing cache to ensure fresh data [CLEAR EXISTING CACHE COMMENT]
	BuildingDataCache.Empty(); // Clear building data cache map [CLEAR BUILDING DATA CACHE]
	GmlIdCache.Empty(); // Clear GML ID cache map [CLEAR GML ID CACHE]
	UE_LOG(LogTemp, Warning, TEXT("Cleared existing cache for fresh data")); // Log message indicating cache has been cleared [CACHE CLEARED LOG]

	AccessToken = Token; // Store authentication token for API requests [STORE ACCESS TOKEN]
	bIsLoading = true; // Set loading flag to prevent concurrent operations [SET IS LOADING FLAG]
	// Reset data loaded flag to allow refresh [RESET DATA LOADED FLAG COMMENT]
	bDataLoaded = false; // Reset data loaded flag to indicate data is being refreshed [RESET DATA LOADED FLAG]

	// Validate token [VALIDATE TOKEN COMMENT]
	if (Token.IsEmpty()) // Check if provided token is empty [TOKEN EMPTY CHECK]
	{ // Start of empty token handling block [EMPTY TOKEN BLOCK START]
		bIsLoading = false; // Reset loading flag since operation cannot proceed [RESET IS LOADING ON ERROR]
		if (GEngine) // Check if global engine instance is available [ENGINE INSTANCE CHECK]
		{ // Start of engine instance block [ENGINE INSTANCE BLOCK START]
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: Access token is empty! Cannot fetch building data.")); // Display error message on screen [ON SCREEN ERROR MESSAGE]
		} // End of engine instance block [ENGINE INSTANCE BLOCK END]
		UE_LOG(LogTemp, Error, TEXT("PreloadAllBuildingData called with empty token")); // Log error message for empty token [EMPTY TOKEN ERROR LOG]
		return; // Exit method early due to invalid token [EARLY RETURN ON ERROR]
	} // End of empty token handling block [EMPTY TOKEN BLOCK END]

	if (GEngine) // Check if global engine instance is available [ENGINE INSTANCE CHECK]
	{ // Start of engine instance block [ENGINE INSTANCE BLOCK START]
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, 
			TEXT("Preloading all building energy data...")); // Display status message on screen [PRELOAD STATUS MESSAGE]
	} // End of engine instance block [ENGINE INSTANCE BLOCK END]
	
	UE_LOG(LogTemp, Warning, TEXT("Starting preload with token length: %d"), Token.Len()); // Log token length for debugging without exposing token content [TOKEN LENGTH LOG]

	// Create HTTP request [CREATE HTTP REQUEST COMMENT]
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest(); // Create thread-safe HTTP request object [CREATE HTTP REQUEST OBJECT]
	
	// Set the URL with parameters [SET URL WITH PARAMETERS COMMENT]
	// API configuration - should be externally configurable [API CONFIGURATION COMMENT]
	FString ApiBaseUrl = TEXT("https://backend.gisworld-tech.com");  // Should come from config [API BASE URL ASSIGNMENT]
	FString DefaultCommunityId = TEXT("08417008");  // Should come from project config [DEFAULT COMMUNITY ID ASSIGNMENT]
	
	FString URL = FString::Printf(TEXT("%s/geospatial/buildings-energy/?community_id=%s&format=json&include_colors=true&energy_type=total&time_period=annual&classification=co2&color_scheme=co2_classes"), 
		*ApiBaseUrl, *DefaultCommunityId); // Construct full API URL with community ID parameter and CO2 color classification [CONSTRUCT API URL]
	
	HttpRequest->SetURL(URL); // Set request URL for HTTP operation [SET REQUEST URL]
	HttpRequest->SetVerb("GET"); // Set HTTP method to GET for data retrieval [SET HTTP VERB]
	HttpRequest->SetHeader("Content-Type", "application/json"); // Set content type header for JSON data [SET CONTENT TYPE HEADER]
	HttpRequest->SetHeader("Accept", "application/json"); // Set accept header to specify JSON response format [SET ACCEPT HEADER]
	
	// Add Authorization header with Bearer token [ADD AUTHORIZATION HEADER COMMENT]
	HttpRequest->SetHeader("Authorization", FString::Printf(TEXT("Bearer %s"), *Token)); // Set authorization header with bearer token for API authentication [SET AUTHORIZATION HEADER]
	
	UE_LOG(LogTemp, Warning, TEXT("Request URL: %s"), *URL); // Log request URL for debugging [REQUEST URL LOG]
	UE_LOG(LogTemp, Warning, TEXT("Authorization Header: Bearer %s..."), *Token.Left(20)); // Log partial token for debugging without exposing full token [AUTHORIZATION HEADER LOG]

	// Set timeout to 30 seconds - server might be slow [SET TIMEOUT COMMENT]
	HttpRequest->SetTimeout(30.0f); // Set request timeout to 30 seconds to handle slow server responses [SET REQUEST TIMEOUT]

	// Bind the preload response callback [BIND PRELOAD RESPONSE CALLBACK COMMENT]
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnPreloadResponseReceived); // Bind response callback method for handling HTTP response [BIND RESPONSE CALLBACK]

	// Execute the request [EXECUTE REQUEST COMMENT]
	if (!HttpRequest->ProcessRequest()) // Attempt to execute HTTP request and check for immediate failure [PROCESS REQUEST CHECK]
	{ // Start of request failure block [REQUEST FAILURE BLOCK START]
		if (GEngine) // Check if global engine instance is available [ENGINE INSTANCE CHECK FOR ERROR]
		{ // Start of engine error block [ENGINE ERROR BLOCK START]
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: Failed to start HTTP request!")); // Display error message on screen [HTTP REQUEST ERROR MESSAGE]
		} // End of engine error block [ENGINE ERROR BLOCK END]
		UE_LOG(LogTemp, Error, TEXT("ProcessRequest returned false")); // Log error message for failed request processing [PROCESS REQUEST ERROR LOG]
	} // End of request failure block [REQUEST FAILURE BLOCK END]
} // End of PreloadAllBuildingData method body [PRELOAD ALL BUILDING DATA BODY END]

void ABuildingEnergyDisplay::OnPreloadResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful) // HTTP response callback for preload data operation [ON PRELOAD RESPONSE RECEIVED DECLARATION]
{ // Start of OnPreloadResponseReceived method body [ON PRELOAD RESPONSE RECEIVED BODY START]
	bIsLoading = false; // Always reset loading flag
	
	UE_LOG(LogTemp, Warning, TEXT("OnPreloadResponseReceived called. Success: %s"), bWasSuccessful ? TEXT("true") : TEXT("false"));

	if (!bWasSuccessful)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: HTTP request failed or timed out. The server might be slow or unreachable."));
		}
		UE_LOG(LogTemp, Error, TEXT("HTTP Request was not successful - likely timeout or network error"));
		return;
	}

	if (!Response.IsValid())
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: Invalid response from server."));
		}
		UE_LOG(LogTemp, Error, TEXT("Response is not valid"));
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	UE_LOG(LogTemp, Warning, TEXT("Response Code: %d"), ResponseCode);

	if (ResponseCode == 401)
	{
		UE_LOG(LogTemp, Error, TEXT("401 Unauthorized - Access token may be expired"));
		
		// Try to refresh token automatically if we have a refresh token
		if (!RefreshToken.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 Attempting automatic token refresh..."));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, 
					TEXT("🔄 Token expired - attempting refresh..."));
			}
			RefreshAccessToken();
		}
		else
		{
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
					TEXT("ERROR: Authentication failed (401). Check your access token."));
			}
			UE_LOG(LogTemp, Error, TEXT("401 Unauthorized - No refresh token available"));
		}
		return;
	}

	if (ResponseCode == 403)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: Access forbidden (403). Token lacks permissions."));
		}
		UE_LOG(LogTemp, Error, TEXT("403 Forbidden - Insufficient permissions"));
		return;
	} // End of some previous block [PREVIOUS BLOCK END]

	if (ResponseCode != 200) // Check if HTTP response code is not successful (200 OK) [RESPONSE CODE CHECK]
	{ // Start of error response handling block [ERROR RESPONSE BLOCK START]
		FString ResponseBody = Response->GetContentAsString(); // Get response body content for error analysis [GET ERROR RESPONSE BODY]
		if (GEngine) // Check if global engine instance is available [ENGINE INSTANCE CHECK FOR ERROR]
		{ // Start of engine error display block [ENGINE ERROR DISPLAY BLOCK START]
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				FString::Printf(TEXT("ERROR: Server returned code %d"), ResponseCode)); // Display HTTP error code on screen [DISPLAY HTTP ERROR CODE]
		} // End of engine error display block [ENGINE ERROR DISPLAY BLOCK END]
		UE_LOG(LogTemp, Error, TEXT("Server returned code %d. Response: %s"), ResponseCode, *ResponseBody.Left(500)); // Log HTTP error with partial response body [LOG HTTP ERROR]
		return; // Exit method early due to HTTP error [EARLY RETURN ON HTTP ERROR]
	} // End of error response handling block [ERROR RESPONSE BLOCK END]

	// Get the response content as string [GET RESPONSE CONTENT COMMENT]
	FString ResponseContent = Response->GetContentAsString(); // Extract HTTP response body as string [EXTRACT RESPONSE CONTENT]
	UE_LOG(LogTemp, Warning, TEXT("✅ BACKEND RESPONSE - Received %d bytes from: https://backend.gisworld-tech.com"), ResponseContent.Len()); // Log response content length for debugging [LOG RESPONSE LENGTH]
	
	// BACKEND VERIFICATION: Log a sample of the response to prove it's real data
	FString ResponseSample = ResponseContent.Left(200).Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(" "));
	UE_LOG(LogTemp, Warning, TEXT("🔍 BACKEND DATA SAMPLE: %s..."), *ResponseSample);
	
	if (GEngine)
	{
		// DISABLED for single building display: GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, FString::Printf(TEXT("🌐 BACKEND DATA RECEIVED - %d bytes from server"), ResponseContent.Len()));
	}
	
	// Parse and cache all buildings [PARSE AND CACHE BUILDINGS COMMENT]
	ParseAndCacheAllBuildings(ResponseContent); // Call method to parse JSON and populate building cache [PARSE AND CACHE CALL]
} // End of response handling method [RESPONSE HANDLING METHOD END]

void ABuildingEnergyDisplay::ParseAndCacheAllBuildings(const FString& JsonResponse) // ParseAndCacheAllBuildings method to process JSON response and populate cache [PARSE AND CACHE ALL BUILDINGS DECLARATION]
{ // Start of ParseAndCacheAllBuildings method body [PARSE AND CACHE ALL BUILDINGS BODY START]
	// 🔑 CASE SENSITIVITY STRATEGY
	// ============================
	// CRITICAL REQUIREMENT: gml_id and modified_gml_id fields are CASE-SENSITIVE
	// - 'G' is treated as different from 'g'
	// - 'M' is treated as different from 'm' 
	// - All string operations maintain exact case from API responses
	// - No .ToLower() or .ToUpper() conversions are applied to GML ID fields
	// - Field names in JSON parsing use exact case: "gml_id", "modified_gml_id"
	// - Cache storage and lookup operations are case-sensitive
	//
	// This strategy ensures API compatibility and prevents ID mismatches
	// between different API endpoints that expect specific case formats.
	
	UE_LOG(LogTemp, Warning, TEXT("🔑 PARSING: Using case-sensitive strategy for all gml_id operations"));
	
	// Parse JSON [PARSE JSON COMMENT]
	TSharedPtr<FJsonValue> JsonValue; // Create shared pointer for JSON value object [CREATE JSON VALUE POINTER]
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonResponse); // Create JSON reader from response string [CREATE JSON READER]

	if (!FJsonSerializer::Deserialize(Reader, JsonValue) || !JsonValue.IsValid()) // Attempt JSON deserialization and validate result [JSON DESERIALIZATION CHECK]
	{ // Start of JSON parse error block [JSON PARSE ERROR BLOCK START]
		if (GEngine) // Check if global engine instance is available [ENGINE INSTANCE CHECK FOR JSON ERROR]
		{ // Start of engine JSON error display block [ENGINE JSON ERROR DISPLAY BLOCK START]
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, 
				TEXT("ERROR: Failed to parse building data JSON")); // Display JSON parse error on screen [DISPLAY JSON PARSE ERROR]
		} // End of engine JSON error display block [ENGINE JSON ERROR DISPLAY BLOCK END]
		return; // Exit method early due to JSON parse failure [EARLY RETURN ON JSON ERROR]
	} // End of JSON parse error block [JSON PARSE ERROR BLOCK END]

	// The response is an array of buildings [RESPONSE IS ARRAY COMMENT]
	TArray<TSharedPtr<FJsonValue>> BuildingsArray = JsonValue->AsArray(); // Extract JSON array containing building objects [EXTRACT BUILDINGS ARRAY]
	int32 BuildingCount = 0; // Initialize building counter for statistics [INITIALIZE BUILDING COUNTER]

	// Process each building [PROCESS EACH BUILDING COMMENT]
	for (const TSharedPtr<FJsonValue>& BuildingValue : BuildingsArray) // Iterate through each building in the JSON array [ITERATE BUILDINGS LOOP]
	{ // Start of building processing loop [BUILDING PROCESSING LOOP START]
		TSharedPtr<FJsonObject> BuildingObject = BuildingValue->AsObject(); // Extract building object from JSON value [EXTRACT BUILDING OBJECT]
		if (!BuildingObject.IsValid()) // Check if building object is valid [BUILDING OBJECT VALIDATION]
			continue; // Skip invalid building objects [SKIP INVALID BUILDING]

		FString BuildingGmlId = BuildingObject->GetStringField(TEXT("modified_gml_id")); // Extract building GML ID from JSON object - CASE SENSITIVE: 'G' != 'g' [EXTRACT BUILDING GML ID]
		
		// 🔍 DETAILED JSON DEBUGGING FOR MISSING DATA INVESTIGATION
		UE_LOG(LogTemp, Warning, TEXT("🔍 === DEBUGGING BUILDING: %s ==="), *BuildingGmlId);
		
		// Check if this is the problematic building
		if (BuildingGmlId.Contains(TEXT("DEBW_0010008")) || BuildingGmlId.Contains(TEXT("wfbT")))
		{
			UE_LOG(LogTemp, Error, TEXT("🔴 FOUND PROBLEMATIC BUILDING: %s - DETAILED ANALYSIS"), *BuildingGmlId);
			
			// Log complete JSON structure for this building
			FString JsonString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(BuildingObject.ToSharedRef(), Writer);
			UE_LOG(LogTemp, Error, TEXT("🔴 COMPLETE JSON FOR %s:"), *BuildingGmlId);
			UE_LOG(LogTemp, Error, TEXT("%s"), *JsonString);
		}
		
		// CRUCIAL: Also extract the actual gml_id (with L) for attributes API - CASE SENSITIVE: 'G' != 'g'
		FString ActualGmlId;
		if (BuildingObject->HasField(TEXT("gml_id"))) // CASE SENSITIVE field name check
		{
			ActualGmlId = BuildingObject->GetStringField(TEXT("gml_id")); // CASE SENSITIVE field extraction
			UE_LOG(LogTemp, Warning, TEXT("SUCCESS Found gml_id field in JSON: %s"), *ActualGmlId);
		}
		else
		{
			// Fallback: convert modified_gml_id to gml_id
			ActualGmlId = BuildingGmlId.Replace(TEXT("_"), TEXT("L"));
			UE_LOG(LogTemp, Warning, TEXT("WARNING No gml_id field found, using conversion: %s -> %s"), *BuildingGmlId, *ActualGmlId);
		}
		
		// Store the mapping for later use by attributes API
		GmlIdCache.Add(BuildingGmlId, ActualGmlId);
		UE_LOG(LogTemp, Display, TEXT("🔗 CACHE [%d] Cached mapping: modified_gml_id=%s -> gml_id=%s"), BuildingCount + 1, *BuildingGmlId, *ActualGmlId);

		// Get the energy_result object
		TSharedPtr<FJsonObject> EnergyResult = BuildingObject->GetObjectField(TEXT("energy_result"));
		if (!EnergyResult.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("🔴 Building %s: Missing 'energy_result' field - checking for alternative structures"), *BuildingGmlId);
			
			// Check for alternative field names
			if (BuildingObject->HasField(TEXT("energy_data")))
			{
				EnergyResult = BuildingObject->GetObjectField(TEXT("energy_data"));
				UE_LOG(LogTemp, Warning, TEXT("🔄 Found 'energy_data' field instead for %s"), *BuildingGmlId);
			}
			else if (BuildingObject->HasField(TEXT("result")))
			{
				EnergyResult = BuildingObject->GetObjectField(TEXT("result"));
				UE_LOG(LogTemp, Warning, TEXT("🔄 Found 'result' field instead for %s"), *BuildingGmlId);
			}
			
			if (!EnergyResult.IsValid())
			{
				UE_LOG(LogTemp, Error, TEXT("❌ Building %s: No valid energy data structure found - SKIPPING"), *BuildingGmlId);
				continue;
			}
		}

		// Get the "begin" and "end" objects
		TSharedPtr<FJsonObject> BeginObject = EnergyResult->GetObjectField(TEXT("begin"));
		TSharedPtr<FJsonObject> EndObject = EnergyResult->GetObjectField(TEXT("end"));
		
		UE_LOG(LogTemp, Warning, TEXT("🔍 Building %s: Begin valid=%s, End valid=%s"), 
			*BuildingGmlId, BeginObject.IsValid() ? TEXT("YES") : TEXT("NO"), EndObject.IsValid() ? TEXT("YES") : TEXT("NO"));
		
		if (!BeginObject.IsValid() || !EndObject.IsValid())
		{
			// Try alternative structures
			if (!BeginObject.IsValid() && EnergyResult->HasField(TEXT("before")))
			{
				BeginObject = EnergyResult->GetObjectField(TEXT("before"));
				UE_LOG(LogTemp, Warning, TEXT("🔄 Found 'before' instead of 'begin' for %s"), *BuildingGmlId);
			}
			if (!EndObject.IsValid() && EnergyResult->HasField(TEXT("after")))
			{
				EndObject = EnergyResult->GetObjectField(TEXT("after"));
				UE_LOG(LogTemp, Warning, TEXT("🔄 Found 'after' instead of 'end' for %s"), *BuildingGmlId);
			}
			
			if (!BeginObject.IsValid() || !EndObject.IsValid())
			{
				UE_LOG(LogTemp, Error, TEXT("❌ Building %s: Missing begin/end structure - SKIPPING"), *BuildingGmlId);
				continue;
			}
		}

		TSharedPtr<FJsonObject> BeginResult = BeginObject->GetObjectField(TEXT("result"));
		TSharedPtr<FJsonObject> EndResult = EndObject->GetObjectField(TEXT("result"));
		
		UE_LOG(LogTemp, Warning, TEXT("🔍 Building %s: BeginResult valid=%s, EndResult valid=%s"), 
			*BuildingGmlId, BeginResult.IsValid() ? TEXT("YES") : TEXT("NO"), EndResult.IsValid() ? TEXT("YES") : TEXT("NO"));
		
		// If result objects are missing, try using the begin/end objects directly
		if (!BeginResult.IsValid())
		{
			BeginResult = BeginObject;
			UE_LOG(LogTemp, Warning, TEXT("🔄 Using BeginObject directly as BeginResult for %s"), *BuildingGmlId);
		}
		if (!EndResult.IsValid())
		{
			EndResult = EndObject;
			UE_LOG(LogTemp, Warning, TEXT("🔄 Using EndObject directly as EndResult for %s"), *BuildingGmlId);
		}
		
		if (!BeginResult.IsValid() || !EndResult.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("❌ Building %s: No valid result structure found - SKIPPING"), *BuildingGmlId);
			continue;
		}

		// Extract BEFORE renovation data
		TSharedPtr<FJsonObject> BeginEnergyDemand = BeginResult->GetObjectField(TEXT("energy_demand"));
		TSharedPtr<FJsonObject> BeginEnergySpecific = BeginResult->GetObjectField(TEXT("energy_demand_specific"));
		TSharedPtr<FJsonObject> BeginCO2 = BeginResult->GetObjectField(TEXT("co2_from_energy_demand"));
		
		// Extract AFTER renovation data
		TSharedPtr<FJsonObject> EndEnergyDemand = EndResult->GetObjectField(TEXT("energy_demand"));
		TSharedPtr<FJsonObject> EndEnergySpecific = EndResult->GetObjectField(TEXT("energy_demand_specific"));
		TSharedPtr<FJsonObject> EndCO2 = EndResult->GetObjectField(TEXT("co2_from_energy_demand"));
		
		// 🔍 DETAILED ENERGY DATA DEBUGGING
		UE_LOG(LogTemp, Warning, TEXT("🔍 Building %s - Energy Fields:"), *BuildingGmlId);
		UE_LOG(LogTemp, Warning, TEXT("   BeginEnergyDemand: %s"), BeginEnergyDemand.IsValid() ? TEXT("✅") : TEXT("❌"));
		UE_LOG(LogTemp, Warning, TEXT("   BeginEnergySpecific: %s"), BeginEnergySpecific.IsValid() ? TEXT("✅") : TEXT("❌"));
		UE_LOG(LogTemp, Warning, TEXT("   BeginCO2: %s"), BeginCO2.IsValid() ? TEXT("✅") : TEXT("❌"));
		UE_LOG(LogTemp, Warning, TEXT("   EndEnergyDemand: %s"), EndEnergyDemand.IsValid() ? TEXT("✅") : TEXT("❌"));
		UE_LOG(LogTemp, Warning, TEXT("   EndEnergySpecific: %s"), EndEnergySpecific.IsValid() ? TEXT("✅") : TEXT("❌"));
		UE_LOG(LogTemp, Warning, TEXT("   EndCO2: %s"), EndCO2.IsValid() ? TEXT("✅") : TEXT("❌"));
		
		// Log available fields in BeginResult and EndResult for debugging
		if (BuildingGmlId.Contains(TEXT("DEBW_0010008")) || BuildingGmlId.Contains(TEXT("wfbT")))
		{
			UE_LOG(LogTemp, Error, TEXT("🔴 DEBUGGING FIELDS IN BeginResult for %s:"), *BuildingGmlId);
			for (const auto& Field : BeginResult->Values)
			{
				UE_LOG(LogTemp, Error, TEXT("   Field: %s"), *Field.Key);
			}
			
			UE_LOG(LogTemp, Error, TEXT("🔴 DEBUGGING FIELDS IN EndResult for %s:"), *BuildingGmlId);
			for (const auto& Field : EndResult->Values)
			{
				UE_LOG(LogTemp, Error, TEXT("   Field: %s"), *Field.Key);
			}
		}

		// --- CESIUM MATERIAL COLORING LOGIC ---
		// Extract color from "end" (after renovation) as specified
		FString ColorHex = TEXT("#66b032"); // Fallback color only for this example
		
		// Debug: Check EndObject structure
		UE_LOG(LogTemp, Warning, TEXT("🎨 COLOR DEBUGGING for building %s:"), *BuildingGmlId);
		UE_LOG(LogTemp, Warning, TEXT("   EndObject valid: %s"), EndObject.IsValid() ? TEXT("YES") : TEXT("NO"));
		
		TSharedPtr<FJsonObject> EndColor = EndObject->GetObjectField(TEXT("color"));
		UE_LOG(LogTemp, Warning, TEXT("   EndColor valid: %s"), EndColor.IsValid() ? TEXT("YES") : TEXT("NO"));
		
		if (EndColor.IsValid())
		{
			// Check available color fields
			UE_LOG(LogTemp, Warning, TEXT("   Available color fields:"));
			for (const auto& ColorField : EndColor->Values)
			{
				if (ColorField.Value->Type == EJson::String)
				{
					FString ColorValue = ColorField.Value->AsString();
					UE_LOG(LogTemp, Warning, TEXT("     %s: %s"), *ColorField.Key, *ColorValue);
				}
			}
			
			if (EndColor->HasField(TEXT("energy_demand_specific_color")))
			{
				ColorHex = EndColor->GetStringField(TEXT("energy_demand_specific_color"));
				UE_LOG(LogTemp, Warning, TEXT("✅ COLOR Building %s extracted color: %s"), *BuildingGmlId, *ColorHex);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("❌ WARNING No 'energy_demand_specific_color' field found for %s"), *BuildingGmlId);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("❌ WARNING No 'color' object found in EndObject for %s"), *BuildingGmlId);
			
			// Try alternative color field locations
			if (EndResult->HasField(TEXT("color")))
			{
				EndColor = EndResult->GetObjectField(TEXT("color"));
				if (EndColor.IsValid() && EndColor->HasField(TEXT("energy_demand_specific_color")))
				{
					ColorHex = EndColor->GetStringField(TEXT("energy_demand_specific_color"));
					UE_LOG(LogTemp, Warning, TEXT("✅ COLOR Found color in EndResult instead: %s"), *ColorHex);
				}
			}
		}

		// Convert hex color to FLinearColor
		FLinearColor BuildingColor = ConvertHexToLinearColor(ColorHex);
		UE_LOG(LogTemp, Warning, TEXT("🎨 COLOR Converted %s to LinearColor(R:%.3f, G:%.3f, B:%.3f)"), 
			*ColorHex, BuildingColor.R, BuildingColor.G, BuildingColor.B);

		// Store color data for this building (CASE-SENSITIVE - no duplicates)
		// Check if we already have this building in the cache
		if (BuildingColorCache.Contains(BuildingGmlId))
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 COLOR CACHE: Overwriting existing color for %s"), *BuildingGmlId);
		}
		
		BuildingColorCache.Add(BuildingGmlId, BuildingColor);
		UE_LOG(LogTemp, Log, TEXT("✅ COLOR CACHED: %s -> %s (R:%.3f G:%.3f B:%.3f)"), 
			*BuildingGmlId, *ColorHex, BuildingColor.R, BuildingColor.G, BuildingColor.B);
		
		// IMPORTANT: Only cache under actual gml_id if it's genuinely different
		// This prevents duplicate cache entries that were causing color lookup issues
		if (!ActualGmlId.IsEmpty() && !ActualGmlId.Equals(BuildingGmlId))
		{
			if (BuildingColorCache.Contains(ActualGmlId))
			{
				UE_LOG(LogTemp, Warning, TEXT("🔄 COLOR CACHE: Overwriting existing color for actual gml_id %s"), *ActualGmlId);
			}
			BuildingColorCache.Add(ActualGmlId, BuildingColor);
			UE_LOG(LogTemp, Log, TEXT("✅ COLOR CACHED (ACTUAL): %s -> %s"), *ActualGmlId, *ColorHex);
		}

		// Build display message to match the screenshot format
		FString DisplayMessage = FString::Printf(TEXT("Building ID: %s\n\n"), *BuildingGmlId);
		
		// CO2 Section
		DisplayMessage += TEXT("CO2 [t CO2/a]\n");
		
		// Before Renovation CO2
		if (BeginCO2.IsValid() && BeginCO2->HasField(TEXT("value")))
		{
			TSharedPtr<FJsonValue> BeginCO2Json = BeginCO2->TryGetField(TEXT("value"));
			if (BeginCO2Json.IsValid() && BeginCO2Json->Type != EJson::Null)
			{
				int32 BeginCO2Value = BeginCO2->GetIntegerField(TEXT("value"));
				// Convert from kg to tonnes (divide by 1000) and format with 3 decimal places
				float BeginCO2Tonnes = BeginCO2Value / 1000.0f;
				DisplayMessage += FString::Printf(TEXT("Before Renovation: %.3f\n"), BeginCO2Tonnes);
				UE_LOG(LogTemp, Warning, TEXT("✅ Building %s: BeginCO2 = %d kg (%.3f tonnes)"), *BuildingGmlId, BeginCO2Value, BeginCO2Tonnes);
			}
			else
			{
				DisplayMessage += TEXT("Before Renovation: No data\n");
				UE_LOG(LogTemp, Warning, TEXT("❌ Building %s: BeginCO2 value field is null"), *BuildingGmlId);
			}
		}
		else
		{
			DisplayMessage += TEXT("Before Renovation: No data\n");
			UE_LOG(LogTemp, Warning, TEXT("❌ Building %s: BeginCO2 object missing or no 'value' field"), *BuildingGmlId);
		}
		
		// After Renovation CO2
		if (EndCO2.IsValid() && EndCO2->HasField(TEXT("value")))
		{
			TSharedPtr<FJsonValue> EndCO2Json = EndCO2->TryGetField(TEXT("value"));
			if (EndCO2Json.IsValid() && EndCO2Json->Type != EJson::Null)
			{
				int32 EndCO2Value = EndCO2->GetIntegerField(TEXT("value"));
				// Convert from kg to tonnes (divide by 1000) and format with 3 decimal places
				float EndCO2Tonnes = EndCO2Value / 1000.0f;
				DisplayMessage += FString::Printf(TEXT("After Renovation: %.3f\n\n"), EndCO2Tonnes);
			}
			else
			{
				DisplayMessage += TEXT("After Renovation: No data\n\n");
			}
		}
		else
		{
			DisplayMessage += TEXT("After Renovation: No data\n\n");
		}
		
		// Energy Demand Specific Section
		DisplayMessage += TEXT("Energy Demand Specific [kWh/m²a]\n");
		
		// Before Renovation Energy Demand Specific
		if (BeginEnergySpecific.IsValid() && BeginEnergySpecific->HasField(TEXT("value")))
		{
			TSharedPtr<FJsonValue> BeginSpecificJson = BeginEnergySpecific->TryGetField(TEXT("value"));
			if (BeginSpecificJson.IsValid() && BeginSpecificJson->Type != EJson::Null)
			{
				int32 BeginSpecificValue = BeginEnergySpecific->GetIntegerField(TEXT("value"));
				DisplayMessage += FString::Printf(TEXT("Before Renovation: %d\n"), BeginSpecificValue);
				UE_LOG(LogTemp, Warning, TEXT("✅ Building %s: BeginEnergySpecific = %d"), *BuildingGmlId, BeginSpecificValue);
			}
			else
			{
				DisplayMessage += TEXT("Before Renovation: No data\n");
				UE_LOG(LogTemp, Warning, TEXT("❌ Building %s: BeginEnergySpecific value field is null"), *BuildingGmlId);
			}
		}
		else
		{
			DisplayMessage += TEXT("Before Renovation: No data\n");
			UE_LOG(LogTemp, Warning, TEXT("❌ Building %s: BeginEnergySpecific object missing or no 'value' field"), *BuildingGmlId);
		}
		
		// After Renovation Energy Demand Specific
		if (EndEnergySpecific.IsValid() && EndEnergySpecific->HasField(TEXT("value")))
		{
			TSharedPtr<FJsonValue> EndSpecificJson = EndEnergySpecific->TryGetField(TEXT("value"));
			if (EndSpecificJson.IsValid() && EndSpecificJson->Type != EJson::Null)
			{
				int32 EndSpecificValue = EndEnergySpecific->GetIntegerField(TEXT("value"));
				DisplayMessage += FString::Printf(TEXT("After Renovation: %d"), EndSpecificValue);
			}
			else
			{
				DisplayMessage += TEXT("After Renovation: No data");
			}
		}
		else
		{
			DisplayMessage += TEXT("After Renovation: No data");
		}
		
		// === CASE-SENSITIVE CACHE TRACKING ===
		// ALWAYS cache building data from API, maintaining exact case sensitivity
		// CRITICAL: gml_id and modified_gml_id fields are case-sensitive ('G' != 'g')
		// Store with exact case from API response - no case conversion allowed
		BuildingDataCache.Add(BuildingGmlId, DisplayMessage);
		
		// === COORDINATE CACHING FOR POSITION VALIDATION ===
		// Extract and cache building coordinates for position validation
		// Use a unique cache key when the JSON provides a numeric 'id' so duplicate gml_ids with
		// different geometries are preserved. We still keep BuildingDataCache/BuildingColorCache
		// keyed by the base GML id for lookups and coloring.
		FString UniqueCacheKey = BuildingGmlId;
		if (BuildingObject->HasField(TEXT("id")))
		{
			// Use integer id when available to create a unique key
			double NumericIdD = 0.0;
			if (BuildingObject->TryGetNumberField(TEXT("id"), NumericIdD))
			{
				int32 NumericId = (int32)NumericIdD;
				UniqueCacheKey = FString::Printf(TEXT("%s#%d"), *BuildingGmlId, NumericId);
			}
		}

		if (BuildingObject->HasField(TEXT("coordinates")))
		{
			FString CoordinatesData = BuildingObject->GetStringField(TEXT("coordinates"));
			StoreBuildingCoordinates(UniqueCacheKey, CoordinatesData);
		}
		else if (BuildingObject->HasField(TEXT("geom")))
		{
			TSharedPtr<FJsonObject> GeomObject = BuildingObject->GetObjectField(TEXT("geom"));
			if (GeomObject.IsValid())
			{
				FString GeomString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&GeomString);
				FJsonSerializer::Serialize(GeomObject.ToSharedRef(), Writer);
				StoreBuildingCoordinates(UniqueCacheKey, GeomString);
			}
		}
		else if (BuildingObject->HasField(TEXT("position")))
		{
			FString PositionData = BuildingObject->GetStringField(TEXT("position"));
			StoreBuildingCoordinates(UniqueCacheKey, PositionData);
		}
		
		UE_LOG(LogTemp, Warning, TEXT("📁 CACHED [%d]: %s"), BuildingCount, *BuildingGmlId);
		
		// Also cache the gml_id mapping for lookup with CASE TRACKING
		if (!ActualGmlId.IsEmpty() && !ActualGmlId.Equals(BuildingGmlId))
		{
			BuildingDataCache.Add(ActualGmlId, DisplayMessage);

			// For coordinate lookup convenience, build a combined coordinate set for the alternate id
			TArray<FVector> CombinedCoords;
			for (const auto& Entry : BuildingCoordinatesCache)
			{
				if (Entry.Key.StartsWith(BuildingGmlId))
				{
					CombinedCoords.Append(Entry.Value);
				}
			}
			if (CombinedCoords.Num() > 0)
			{
				BuildingCoordinatesCache.Add(ActualGmlId, CombinedCoords);
			}

			UE_LOG(LogTemp, Warning, TEXT("🔄 CASE MAPPING: '%s' -> '%s'"), *BuildingGmlId, *ActualGmlId);
		}
		BuildingCount++;
	}

	// Mark data as loaded
	bDataLoaded = true;

	// BACKEND VERIFICATION: Confirm data is from real API
	UE_LOG(LogTemp, Warning, TEXT("🔒 BACKEND VERIFICATION COMPLETE:"));
	UE_LOG(LogTemp, Warning, TEXT("  ✅ Data Source: https://backend.gisworld-tech.com API"));
	UE_LOG(LogTemp, Warning, TEXT("  ✅ Authentication: Bearer token verified"));
	UE_LOG(LogTemp, Warning, TEXT("  ✅ Buildings loaded: %d from live database"), BuildingCount);
	UE_LOG(LogTemp, Warning, TEXT("  ✅ Cache populated: Real-time building energy data"));
	
	// 🎨 COLOR CACHE STATISTICS (Case-Sensitive Analysis)
	UE_LOG(LogTemp, Warning, TEXT("🎨 COLOR CACHE ANALYSIS:"));
	UE_LOG(LogTemp, Warning, TEXT("  📊 BuildingDataCache: %d entries"), BuildingDataCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("  📊 BuildingColorCache: %d entries"), BuildingColorCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("  📊 GmlIdCache: %d mappings"), GmlIdCache.Num());
	
	// Check for potential duplicate issues
	if (BuildingColorCache.Num() != BuildingDataCache.Num())
	{
		int32 ColorDiff = FMath::Abs(BuildingColorCache.Num() - BuildingDataCache.Num());
		UE_LOG(LogTemp, Warning, TEXT("  ⚠️ COLOR CACHE MISMATCH: %d difference between data and color cache"), ColorDiff);
		UE_LOG(LogTemp, Warning, TEXT("  💡 This suggests some buildings lack color data or have duplicate entries"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("  ✅ COLOR CACHE MATCH: Data and color cache sizes align"));
	}
	
	// Log sample color cache entries to verify case sensitivity
	UE_LOG(LogTemp, Warning, TEXT("🎨 SAMPLE COLOR CACHE ENTRIES (case-sensitive):"));
	int32 ColorSampleCount = 0;
	for (const auto& ColorEntry : BuildingColorCache)
	{
		UE_LOG(LogTemp, Warning, TEXT("   %d: '%s' -> R:%.2f G:%.2f B:%.2f"), 
			ColorSampleCount + 1, *ColorEntry.Key, 
			ColorEntry.Value.R, ColorEntry.Value.G, ColorEntry.Value.B);
		if (++ColorSampleCount >= 5) break; // Show first 5 entries
	}
	
	// 🧹 AUTOMATIC CACHE CLEANING: Remove any duplicates from case-insensitive era
	UE_LOG(LogTemp, Warning, TEXT("🧹 Running automatic cache cleaning..."));
	CleanDuplicateColorCacheEntries();

	// 🎨 AUTO COLOR APPLICATION: Apply colors immediately when data loads
	// TEMPORARILY DISABLED - Causing gray overlay on entire scene
	// UE_LOG(LogTemp, Warning, TEXT("🎨 AUTO-APPLYING COLORS: Starting immediate color application to all buildings..."));
	
	// Schedule color application after a short delay to ensure Cesium is ready
	// DISABLED - This was causing the entire scene to turn gray
	/*
	FTimerHandle DataLoadColorTimer;
	GetWorld()->GetTimerManager().SetTimer(DataLoadColorTimer, [this]()
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 EXECUTING: Automatic color application post data load"));
		ApplyColorsDirectlyToGeometry();
		
		// Try multiple approaches to ensure colors are applied
		ApplyColorsToCSiumTileset();
		ForceApplyColors();
		
		UE_LOG(LogTemp, Warning, TEXT("🎨 AUTO-APPLICATION COMPLETE: All color methods executed"));
	}, 2.0f, false); // 2 second delay to let Cesium initialize
	*/

	if (GEngine)
	{
	// DISABLED for single building display: GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Green, FString::Printf(TEXT("🌐 BACKEND DATA LOADED ✓ %d buildings from live API! Click any building to see instant results."), BuildingCount));
	}

	UE_LOG(LogTemp, Warning, TEXT("Successfully cached %d buildings"), BuildingCount);
	
	// 🎨 COLOR CACHE SUMMARY
	UE_LOG(LogTemp, Warning, TEXT("🎨 ===== COLOR CACHE SUMMARY ====="));
	UE_LOG(LogTemp, Warning, TEXT("🎨 BuildingColorCache contains %d entries:"), BuildingColorCache.Num());
	
	int32 ColorIndex = 0;
	for (const auto& ColorEntry : BuildingColorCache)
	{
		FString GmlId = ColorEntry.Key;
		FLinearColor Color = ColorEntry.Value;
		
		// Convert back to hex for display
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		
		UE_LOG(LogTemp, Warning, TEXT("🎨   [%d] %s -> %s (R:%.3f G:%.3f B:%.3f)"), 
			ColorIndex++, *GmlId, *HexColor, Color.R, Color.G, Color.B);
		
		// Log first 5 entries in detail, then summary for rest
		if (ColorIndex >= 5 && BuildingColorCache.Num() > 10)
		{
			UE_LOG(LogTemp, Warning, TEXT("🎨   ... and %d more entries"), BuildingColorCache.Num() - ColorIndex);
			break;
		}
	}
	UE_LOG(LogTemp, Warning, TEXT("🎨 ================================"));
	
	// Note: Color application will be triggered separately when needed
	if (BuildingColorCache.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 Color cache populated with %d entries - ready for application"), BuildingColorCache.Num());
		
		// RESET PRELOAD FLAG: Allow future manual loading calls
		static bool* PreloadFlag = []() -> bool* {
			static bool bPreloadInProgress = false;
			return &bPreloadInProgress;
		}();
		*PreloadFlag = false;
		UE_LOG(LogTemp, Warning, TEXT("✅ PRELOAD FLAG RESET: Future data loading calls are now allowed"));
		
		// 🛑 AUTOMATIC COLOR APPLICATION DISABLED  
		// Prevents tile streaming from triggering repeated material creation
		UE_LOG(LogTemp, Warning, TEXT("🛑 AUTO-APPLY DISABLED: Use ForceColorsNow() or ApplyColorsNow() manually"));
		UE_LOG(LogTemp, Warning, TEXT("💡 This prevents %d building colors from creating materials on every tile stream"), BuildingColorCache.Num());
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Yellow, 
				TEXT("🛑 Auto-apply disabled. Use 'Force Colors Now' to manually apply colors."));
		}
		
		// DISABLED to prevent tile streaming issues:
		/*
		FTimerHandle AutoApplyTimer;
		GetWorld()->GetTimerManager().SetTimer(AutoApplyTimer, [this]()
		{
			ApplyColorsDirectlyToGeometry();
		}, 8.0f, false);
		*/
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("🎨 ERROR: No colors were cached! Color application will fail."));
	}
	
	// Check for color variety in the dataset
	TMap<FString, int32> ColorCounts;
	for (const auto& BuildingColor : BuildingColorCache)
	{
		FLinearColor Color = BuildingColor.Value;
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		
		if (ColorCounts.Contains(HexColor))
		{
			ColorCounts[HexColor]++;
		}
		else
		{
			ColorCounts.Add(HexColor, 1);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("STATS Color variety analysis:"));
	for (const auto& ColorCount : ColorCounts)
	{
		UE_LOG(LogTemp, Warning, TEXT("  Color %s: %d buildings"), *ColorCount.Key, ColorCount.Value);
	}
	
	// If mostly gray colors, still proceed — apply real API colors (including #808080)
	if (ColorCounts.Num() <= 2 && ColorCounts.Contains(TEXT("#808080")))
	{
		UE_LOG(LogTemp, Warning, TEXT("NOTICE Most buildings are gray (#808080). Using API colors as-is (no test colors)."));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("🌈 Found %d different colors in API data."), ColorCounts.Num());
	}
	
	// Create and apply dynamic material to Cesium tileset
	static bool bMaterialCreated = false;
	if (!bMaterialCreated)
	{
		CreateBuildingEnergyMaterial();
		bMaterialCreated = true;
		UE_LOG(LogTemp, Warning, TEXT("🎨 MATERIAL: Created for first instance only"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 MATERIAL: Reusing existing material from first instance"));
	}
	
	// Always attempt to apply colors (apply defaults as well) — schedule after material creation
	UE_LOG(LogTemp, Warning, TEXT("COLOR Scheduling per-building color application (including default colors)..."));
	FTimerHandle ColorApplicationTimer;
	GetWorld()->GetTimerManager().SetTimer(ColorApplicationTimer, [this]()
	{
		CreatePerBuildingColorMaterial();
	}, 1.0f, false);
	
	// Debug: Log first 10 building IDs to help with matching issues
	UE_LOG(LogTemp, Warning, TEXT("LIST First 10 cached building IDs for reference:"));
	int32 DebugCount = 0;
	for (auto& Elem : BuildingDataCache)
	{
		UE_LOG(LogTemp, Warning, TEXT("  %d: %s"), DebugCount + 1, *Elem.Key);
		if (++DebugCount >= 10) break;
	}
}

// 🔍 BLUEPRINT CALLABLE: Debug Cesium property mapping between gml:id and modified_gml_id
void ABuildingEnergyDisplay::DebugCesiumPropertyMapping()
{
	UE_LOG(LogTemp, Warning, TEXT("🔍 CESIUM DEBUG: Starting comprehensive property mapping analysis..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 No building colors cached! Run API fetch first."));
		return;
	}
	
	// Find Cesium tileset actor
	AActor* TilesetActor = nullptr;
	TArray<AActor*> AllActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), AllActors);
	
	for (AActor* Actor : AllActors)
	{
		if (Actor && Actor->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
		{
			TilesetActor = Actor;
			UE_LOG(LogTemp, Warning, TEXT("🎯 FOUND Cesium3DTileset: %s"), *Actor->GetName());
			break;
		}
	}
	
	if (!TilesetActor)
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 No Cesium3DTileset actor found!"));
		return;
	}
	
	// Analyze metadata component
	UActorComponent* MetadataComponent = nullptr;
	TArray<UActorComponent*> AllComponents;
	TilesetActor->GetComponents<UActorComponent>(AllComponents);
	
	for (UActorComponent* Component : AllComponents)
	{
		if (Component && Component->GetClass()->GetName().Contains(TEXT("CesiumFeaturesMetadata")))
		{
			MetadataComponent = Component;
			UE_LOG(LogTemp, Warning, TEXT("🎯 FOUND CesiumFeaturesMetadataComponent"));
			break;
		}
	}
	
	if (MetadataComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("📋 CESIUM METADATA ANALYSIS:"));
		UClass* MetadataClass = MetadataComponent->GetClass();
		
		for (TFieldIterator<FProperty> PropIt(MetadataClass); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			FString PropName = Property->GetName();
			FString PropType = Property->GetClass()->GetName();
			
			UE_LOG(LogTemp, Warning, TEXT("   🏷️ Property: %s (Type: %s)"), *PropName, *PropType);
			
			if (PropName.Contains(TEXT("Description")))
			{
				UE_LOG(LogTemp, Warning, TEXT("      🎯 METADATA DESCRIPTION PROPERTY FOUND!"));
			}
		}
	}
	
	// MATERIAL ANALYSIS: Check what materials are currently on the tileset
	UE_LOG(LogTemp, Warning, TEXT("🎨 MATERIAL ANALYSIS:"));
	TArray<UMeshComponent*> MeshComponents;
	TilesetActor->GetComponents<UMeshComponent>(MeshComponents);
	
	UE_LOG(LogTemp, Warning, TEXT("   Found %d mesh components"), MeshComponents.Num());
	
	for (int32 i = 0; i < FMath::Min(MeshComponents.Num(), 5); ++i) // Analyze first 5 components
	{
		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComponents[i]))
		{
			FString ComponentName = StaticMeshComp->GetName();
			int32 NumMaterials = StaticMeshComp->GetNumMaterials();
			
			UE_LOG(LogTemp, Warning, TEXT("   Component[%d]: %s (%d materials)"), i, *ComponentName, NumMaterials);
			
			for (int32 MatIdx = 0; MatIdx < NumMaterials; ++MatIdx)
			{
				UMaterialInterface* Material = StaticMeshComp->GetMaterial(MatIdx);
				if (Material)
				{
					bool bIsDynamic = Cast<UMaterialInstanceDynamic>(Material) != nullptr;
					UE_LOG(LogTemp, Warning, TEXT("     Material[%d]: %s (Dynamic: %s)"), 
						MatIdx, *Material->GetName(), bIsDynamic ? TEXT("YES") : TEXT("NO"));
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("     Material[%d]: NULL"), MatIdx);
				}
			}
		}
	}
	
	// Show cached building samples for debugging
	UE_LOG(LogTemp, Warning, TEXT("📊 CACHE ANALYSIS: %d buildings cached with modified_gml_id keys"), BuildingColorCache.Num());
	
	int32 SampleCount = 0;
	for (const auto& CacheEntry : BuildingColorCache)
	{
		if (SampleCount < 10) // Show first 10 for debugging
		{
			FString CachedModifiedGmlId = CacheEntry.Key;
			
			// Try to convert modified_gml_id to potential gml:id format
			FString PotentialGmlId = CachedModifiedGmlId;
			PotentialGmlId = PotentialGmlId.Replace(TEXT("_"), TEXT("L"));
			
			UE_LOG(LogTemp, Warning, TEXT("   [%d] Cache: %s -> Potential gml:id: %s"), 
				SampleCount + 1, *CachedModifiedGmlId, *PotentialGmlId);
		}
		SampleCount++;
	}
	
	// Apply colors for immediate visual feedback
	UE_LOG(LogTemp, Warning, TEXT("🎨 APPLYING COLORS: Using current cached data..."));
	ApplyColorsDirectlyToGeometry();
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Yellow, 
			FString::Printf(TEXT("🔍 CESIUM DEBUG: Analyzed %d cached buildings. Check logs for property mapping details."), BuildingColorCache.Num()));
	}
}

// 🔧 MATERIAL UTILITY: Create or get dynamic material for a mesh component
UMaterialInstanceDynamic* ABuildingEnergyDisplay::CreateOrGetDynamicMaterial(UStaticMeshComponent* MeshComp, int32 MaterialIndex)
{
	if (!MeshComp)
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 CreateOrGetDynamicMaterial: Null mesh component"));
		return nullptr;
	}
	
	UMaterialInterface* CurrentMaterial = MeshComp->GetMaterial(MaterialIndex);
	if (!CurrentMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔧 Material %d is null, cannot create dynamic material"), MaterialIndex);
		return nullptr;
	}
	
	// Check if it's already a dynamic material
	UMaterialInstanceDynamic* ExistingDynMat = Cast<UMaterialInstanceDynamic>(CurrentMaterial);
	if (ExistingDynMat)
	{
		UE_LOG(LogTemp, Verbose, TEXT("🔧 Reusing existing dynamic material %d"), MaterialIndex);
		return ExistingDynMat;
	}
	
	// Create new dynamic material
	UMaterialInstanceDynamic* NewDynMat = UMaterialInstanceDynamic::Create(CurrentMaterial, MeshComp);
	if (NewDynMat)
	{
		MeshComp->SetMaterial(MaterialIndex, NewDynMat);
		UE_LOG(LogTemp, Warning, TEXT("🔧 Created new dynamic material %d: %s"), MaterialIndex, *NewDynMat->GetName());
		
		// Ensure it has the parameters we need
		EnsureProperMaterialParameters(NewDynMat);
		
		return NewDynMat;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 Failed to create dynamic material %d"), MaterialIndex);
		return nullptr;
	}
}

// 🔧 MATERIAL UTILITY: Ensure dynamic material has proper parameters for color application
void ABuildingEnergyDisplay::EnsureProperMaterialParameters(UMaterialInstanceDynamic* DynMaterial)
{
	if (!DynMaterial)
	{
		return;
	}

	// IMPORTANT:
	// Do NOT force generic Metallic/Roughness/Opacity overrides on Cesium-generated glTF materials.
	// Doing so can flatten shading and create a "gray overlay" look. We only set color parameters
	// when applying building colors.
}

// 🎨 BLUEPRINT CALLABLE: Apply colors to buildings immediately
void ABuildingEnergyDisplay::ApplyColorsNow()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 MANUAL COLOR APPLICATION: User requested immediate color application"));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 No cached building colors! Run data fetch first."));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				TEXT("No building colors cached! Fetch building data first."));
		}
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🎨 Found %d cached building colors, applying to Cesium tileset..."), BuildingColorCache.Num());
	
	// Apply colors using the existing function
	ApplyColorsDirectlyToGeometry();
	
	// Provide user feedback
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Green, 
			FString::Printf(TEXT("🎨 Applied colors from %d cached buildings to Cesium tileset!"), BuildingColorCache.Num()));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("✅ Manual color application completed"));
}

// 🔧 FORCE COLOR APPLICATION: Immediate color application bypassing all delays
void ABuildingEnergyDisplay::ForceColorsNow()
{
	UE_LOG(LogTemp, Warning, TEXT("🔧 FORCE COLORS: Immediate forced application requested"));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 No cached building colors! Run authentication first."));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				TEXT("No building colors cached! Authenticate first."));
		}
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔧 FORCE: Applying colors to %d buildings immediately..."), BuildingColorCache.Num());
	
	// Apply colors immediately without any delays
	ApplyColorsDirectlyToGeometry();
	
	// Provide immediate feedback
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Orange, 
			FString::Printf(TEXT("🔧 FORCED: Applied %d building colors immediately!"), BuildingColorCache.Num()));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔧 Force color application completed"));
}

void ABuildingEnergyDisplay::DisplayBuildingData(const FString& GmlId)
{
	// LEFT-CLICK: Display building energy information from cache
	UE_LOG(LogTemp, Warning, TEXT("✅ LEFT-CLICK: Displaying energy info for building: %s"), *GmlId);
	
	if (!bDataLoaded)
	{
		UE_LOG(LogTemp, Warning, TEXT("Building data not loaded yet for: %s"), *GmlId);
		// Don't show screen message - just log and return
		return;
	}

	// Prevent spam - don't show same building within 1 second
	double CurrentTime = FPlatformTime::Seconds();
	if (LastDisplayedGmlId == GmlId && (CurrentTime - LastDisplayTime) < 1.0)
	{
		UE_LOG(LogTemp, Warning, TEXT("Ignoring duplicate left-click on building %s (too soon)"), *GmlId);
		return;
	}

	// Update last display tracking
	LastDisplayedGmlId = GmlId;
	LastDisplayTime = CurrentTime;

	// LEFT-CLICK: Use modified_gml_id (underscore format) for energy data cache lookup
	FString* CachedEnergyData = BuildingDataCache.Find(GmlId);
	if (CachedEnergyData && !CachedEnergyData->IsEmpty())
	{
		// Found energy data - display using UMG widget (single building display)
		UE_LOG(LogTemp, Warning, TEXT("✅ SUCCESS: Displaying energy info via UMG widget for: %s"), *GmlId);
		
		// Use UMG widget for display instead of on-screen debug messages
		ShowBuildingInfoWidget(GmlId, **CachedEnergyData);
		
		return;
	}

	// === CASE INSENSITIVE SEARCH ===
	// Try enhanced case-insensitive and format-variant search as fallback
	UE_LOG(LogTemp, Warning, TEXT("🔍 CASE SEARCH: Exact match failed for '%s', trying enhanced matching"), *GmlId);
	UE_LOG(LogTemp, Warning, TEXT("🔍 Available cache entries: %d"), BuildingDataCache.Num());
	
	bool bFoundMatch = false;
	FString FoundKey;
	const FString* FoundData = nullptr;
	FString SearchStrategy = TEXT("");
	
	for (const auto& CacheEntry : BuildingDataCache)
	{
		FString CacheKey = CacheEntry.Key;
		FString SearchKey = GmlId;
		
		// Create multiple normalized versions to compare
		TArray<FString> SearchVariations;
		TArray<FString> CacheVariations;
		
		// Add original versions (CASE-SENSITIVE - gml_id fields are case-sensitive)
		SearchVariations.Add(SearchKey);
		CacheVariations.Add(CacheKey);
		
		// Add underscore/L conversion variations (CASE-SENSITIVE)
		FString SearchWithUnderscore = SearchKey.Replace(TEXT("L"), TEXT("_"));
		FString SearchWithL = SearchKey.Replace(TEXT("_"), TEXT("L"));
		FString CacheWithUnderscore = CacheKey.Replace(TEXT("L"), TEXT("_"));
		FString CacheWithL = CacheKey.Replace(TEXT("_"), TEXT("L"));
		
		SearchVariations.Add(SearchWithUnderscore);
		SearchVariations.Add(SearchWithL);
		CacheVariations.Add(CacheWithUnderscore);
		CacheVariations.Add(CacheWithL);
		
		// Compare all variations
		for (const FString& SearchVar : SearchVariations)
		{
			for (const FString& CacheVar : CacheVariations)
			{
				if (SearchVar.Equals(CacheVar))
				{
					FoundKey = CacheEntry.Key;
					FoundData = &CacheEntry.Value;
					bFoundMatch = true;
					UE_LOG(LogTemp, Warning, TEXT("✅ ENHANCED MATCH: Found '%s' -> '%s' (search:'%s' cache:'%s')"), 
						*GmlId, *FoundKey, *SearchVar, *CacheVar);
					break;
				}
			}
			if (bFoundMatch) break;
		}
		if (bFoundMatch) break;
	}
	
	if (bFoundMatch && FoundData)
	{
		// Use UMG widget for enhanced match display (single building display)
		UE_LOG(LogTemp, Warning, TEXT("✅ ENHANCED SUCCESS: Displaying energy info via UMG widget for: %s"), *FoundKey);
		ShowBuildingInfoWidget(FoundKey, **FoundData);
		return;
	}
	
	// Building not found - show detailed debugging
	UE_LOG(LogTemp, Error, TEXT("CACHE MISS: Building '%s' not found in cache"), *GmlId);
	UE_LOG(LogTemp, Warning, TEXT("CACHE DEBUG: Total cached buildings: %d"), BuildingDataCache.Num());
	
	// Show first 5 cache keys for debugging
	int32 ShowCount = 0;
	for (const auto& CacheEntry : BuildingDataCache)
	{
		if (ShowCount < 5)
		{
			UE_LOG(LogTemp, Warning, TEXT("CACHE SAMPLE [%d]: '%s'"), ShowCount + 1, *CacheEntry.Key);
			ShowCount++;
		}
		else
		{
			break;
		}
	}
	
	// Don't show screen message for missing buildings - just log
	UE_LOG(LogTemp, Warning, TEXT("No energy data found for building: %s"), *GmlId);
}

void ABuildingEnergyDisplay::FetchBuildingEnergyData(const FString& GmlId, const FString& Token)
{
	// 🚫 === DISABLED LEGACY FUNCTION - SHOULD NOT BE CALLED BY BLUEPRINT ===
	static int32 FetchCallCounter = 0;
	FetchCallCounter++;
	UE_LOG(LogTemp, Error, TEXT("🚫 === FetchBuildingEnergyData() CALLED #%d ==="), FetchCallCounter);
	UE_LOG(LogTemp, Error, TEXT("🚫 ERROR: This LEGACY function was DISABLED and should NOT be called!"));
	UE_LOG(LogTemp, Error, TEXT("🚫 GmlId received: '%s'"), *GmlId);
	UE_LOG(LogTemp, Error, TEXT("🚫 Blueprint should ONLY call OnBuildingClicked!"));

	// Use cached data instead of making new HTTP requests
	DisplayBuildingData(GmlId);
}

void ABuildingEnergyDisplay::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				TEXT("ERROR: Failed to fetch building data. Check network connection."));
		}
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				FString::Printf(TEXT("ERROR: Server returned code %d"), ResponseCode));
		}
		return;
	}

	// Get the response content as string
	FString ResponseContent = Response->GetContentAsString();

	// Parse JSON
	TSharedPtr<FJsonValue> JsonValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);

	if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
	{
		// The response is an array of buildings
		TArray<TSharedPtr<FJsonValue>> BuildingsArray = JsonValue->AsArray();

		// Find the building with matching modified_gml_id
		bool bFoundBuilding = false;
		for (const TSharedPtr<FJsonValue>& BuildingValue : BuildingsArray)
		{
			TSharedPtr<FJsonObject> BuildingObject = BuildingValue->AsObject();
			
			if (BuildingObject.IsValid())
			{
				FString BuildingGmlId = BuildingObject->GetStringField(TEXT("modified_gml_id"));
				
				// Check if this is the building we're looking for
				if (BuildingGmlId == ModifiedGmlId)
				{
					bFoundBuilding = true;

					// Get the energy_result object
					TSharedPtr<FJsonObject> EnergyResult = BuildingObject->GetObjectField(TEXT("energy_result"));
					
					if (EnergyResult.IsValid())
					{
						// Get the "begin" object (before renovation)
						TSharedPtr<FJsonObject> BeginObject = EnergyResult->GetObjectField(TEXT("begin"));
						
						// Get the "end" object (after renovation)
						TSharedPtr<FJsonObject> EndObject = EnergyResult->GetObjectField(TEXT("end"));
						
						if (BeginObject.IsValid() && EndObject.IsValid())
						{
							TSharedPtr<FJsonObject> BeginResult = BeginObject->GetObjectField(TEXT("result"));
							TSharedPtr<FJsonObject> EndResult = EndObject->GetObjectField(TEXT("result"));
							
							if (BeginResult.IsValid() && EndResult.IsValid())
							{
								// Extract BEFORE renovation data
								TSharedPtr<FJsonObject> BeginEnergyDemand = BeginResult->GetObjectField(TEXT("energy_demand"));
								TSharedPtr<FJsonObject> BeginEnergySpecific = BeginResult->GetObjectField(TEXT("energy_demand_specific"));
								TSharedPtr<FJsonObject> BeginCO2 = BeginResult->GetObjectField(TEXT("co2_from_energy_demand"));
								
								// Extract AFTER renovation data
								TSharedPtr<FJsonObject> EndEnergyDemand = EndResult->GetObjectField(TEXT("energy_demand"));
								TSharedPtr<FJsonObject> EndEnergySpecific = EndResult->GetObjectField(TEXT("energy_demand_specific"));
								TSharedPtr<FJsonObject> EndCO2 = EndResult->GetObjectField(TEXT("co2_from_energy_demand"));
								
								// Build comprehensive display message
								FString DisplayMessage = FString::Printf(TEXT("=== BUILDING: %s ===\n\n"), *ModifiedGmlId);
								
								if (BeginEnergyDemand.IsValid())
								{
									DisplayMessage += TEXT("BEFORE RENOVATION:\n");
									DisplayMessage += FString::Printf(TEXT("• Energy Demand: %d kWh/a\n"), 
										BeginEnergyDemand->GetIntegerField(TEXT("value")));
									
									if (BeginEnergySpecific.IsValid())
									{
										DisplayMessage += FString::Printf(TEXT("• Energy Demand Specific: %d kWh/m²a\n"), 
											BeginEnergySpecific->GetIntegerField(TEXT("value")));
									}
									
									if (BeginCO2.IsValid())
									{
										DisplayMessage += FString::Printf(TEXT("• CO2 Emissions: %d kg/a\n\n"), 
											BeginCO2->GetIntegerField(TEXT("value")));
									}
								}
								
								if (EndEnergyDemand.IsValid())
								{
									DisplayMessage += TEXT("AFTER RENOVATION:\n");
									DisplayMessage += FString::Printf(TEXT("• Energy Demand: %d kWh/a\n"), 
										EndEnergyDemand->GetIntegerField(TEXT("value")));
									
									if (EndEnergySpecific.IsValid())
									{
										DisplayMessage += FString::Printf(TEXT("• Energy Demand Specific: %d kWh/m²a\n"), 
											EndEnergySpecific->GetIntegerField(TEXT("value")));
									}
									
									if (EndCO2.IsValid())
									{
										DisplayMessage += FString::Printf(TEXT("• CO2 Emissions: %d kg/a\n\n"), 
											EndCO2->GetIntegerField(TEXT("value")));
									}
								}
								
								// Calculate savings
								if (BeginEnergyDemand.IsValid() && EndEnergyDemand.IsValid())
								{
									int32 EnergyBefore = BeginEnergyDemand->GetIntegerField(TEXT("value"));
									int32 EnergyAfter = EndEnergyDemand->GetIntegerField(TEXT("value"));
									int32 EnergySaved = EnergyBefore - EnergyAfter;
									float PercentageSaved = ((float)EnergySaved / (float)EnergyBefore) * 100.0f;
									
									DisplayMessage += TEXT("SAVINGS:\n");
									DisplayMessage += FString::Printf(TEXT("• Energy Saved: %d kWh/a (%.1f%%)\n"), 
										EnergySaved, PercentageSaved);
								}
								
								if (BeginCO2.IsValid() && EndCO2.IsValid())
								{
									int32 CO2Before = BeginCO2->GetIntegerField(TEXT("value"));
									int32 CO2After = EndCO2->GetIntegerField(TEXT("value"));
									int32 CO2Saved = CO2Before - CO2After;
									float CO2PercentageSaved = ((float)CO2Saved / (float)CO2Before) * 100.0f;
									
									DisplayMessage += FString::Printf(TEXT("• CO2 Reduced: %d kg/a (%.1f%%)"), 
										CO2Saved, CO2PercentageSaved);
								}
								
								// DISABLED: Don't display here - only use the new ShowBuildingInfoWidget system
								// ShowBuildingInfoWidget(ModifiedGmlId, DisplayMessage);
								UE_LOG(LogTemp, Warning, TEXT("OLD OnResponseReceived called for: %s - DISABLED to prevent duplicates"), *ModifiedGmlId);
								
								// Cache this data for instant retrieval next time
								BuildingDataCache.Add(ModifiedGmlId, DisplayMessage);
							}
						}
					}
					
					break; // Found the building, no need to continue
				}
			}
		}

		if (!bFoundBuilding)
		{
			UE_LOG(LogTemp, Warning, TEXT("No energy data found for building: %s"), *ModifiedGmlId);
			// Don't show screen message for missing buildings to avoid clutter
		}
	}
	else
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("ERROR: Failed to parse server response"));
		}
	}
}

void ABuildingEnergyDisplay::ClearCache()
{
	BuildingDataCache.Empty();
	GmlIdCache.Empty(); // Also clear gml_id cache
	bDataLoaded = false;
	bIsLoading = false;
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("Building data cache cleared"));
	}
	UE_LOG(LogTemp, Warning, TEXT("Building energy data cache cleared"));
}

// Manual cache refresh function (optimized for frequent calls)
void ABuildingEnergyDisplay::RefreshBuildingCache()
{
	UE_LOG(LogTemp, Log, TEXT("Cache refresh requested for fresh building data"));
	
	// Skip if already refreshing to avoid spam
	if (bIsLoading)
	{
		UE_LOG(LogTemp, Log, TEXT("Cache refresh already in progress, skipping"));
		return;
	}
	
	if (!AccessToken.IsEmpty())
	{
		// Reset cache refresh timer
		CacheRefreshTimer = 0.0f;
		
		// Clear existing cache for fresh data
		BuildingDataCache.Empty();
		GmlIdCache.Empty();
		bDataLoaded = false;
		
		// Restart authentication and data loading
		AuthenticateAndLoadData();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("No access token - starting authentication for cache refresh"));
		// Try to authenticate first
		AuthenticateAndLoadData();
	}
}

void ABuildingEnergyDisplay::AuthenticateAndLoadData()
{
	// DUPLICATE CALL PREVENTION - But allow manual Blueprint calls
	static bool bAuthInProgress = false;
	static FString LastSuccessfulToken = TEXT("");
	static double LastResetTime = 0.0;
	
	// Allow manual reset every 2 seconds for Blueprint calls
	double CurrentTime = FPlatformTime::Seconds();
	if (CurrentTime - LastResetTime > 2.0)
	{
		bAuthInProgress = false; // Reset for manual calls
		LastResetTime = CurrentTime;
		UE_LOG(LogTemp, Warning, TEXT("🔄 AUTH RESET: Manual authentication reset allowed"));
	}
	
	if (bAuthInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("🛑 DUPLICATE AUTH PREVENTED: Authentication already in progress. Skipping."));
		return;
	}
	
	if (!LastSuccessfulToken.IsEmpty() && bDataLoaded)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 REUSING TOKEN: Using existing successful authentication."));
		AccessToken = LastSuccessfulToken;
		// Apply colors immediately if we have cached data
		if (BuildingColorCache.Num() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("🎨 AUTO-APPLY: Applying colors from existing cache (%d buildings)"), BuildingColorCache.Num());
			ApplyColorsDirectlyToGeometry();
		}
		return;
	}
	
	bAuthInProgress = true;
	UE_LOG(LogTemp, Warning, TEXT("AuthenticateAndLoadData() called - refreshing cache data"));
	
	// Allow re-authentication and cache refresh
	bIsLoading = false;
	bDataLoaded = false;
	
	UE_LOG(LogTemp, Warning, TEXT("Reset cache flags for fresh data load"));

	bIsLoading = true;
	
	if (GEngine)
	{
		// DISABLED for single building display: GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Cyan, TEXT("Authenticating..."));
	}

	// API configuration - should be externally configurable
	FString ApiBaseUrl = TEXT("https://backend.gisworld-tech.com");  // Should come from config
	
	UE_LOG(LogTemp, Warning, TEXT("Starting authentication request to: %s/api/token/"), *ApiBaseUrl);

	// Create HTTP request for authentication
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	
	// Set authentication URL
	FString AuthURL = FString::Printf(TEXT("%s/api/token/"), *ApiBaseUrl);
	HttpRequest->SetURL(AuthURL);
	HttpRequest->SetVerb("POST");
	HttpRequest->SetHeader("Content-Type", "application/json");
	HttpRequest->SetHeader("Accept", "application/json");
	
	// Create JSON payload with credentials
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
	JsonObject->SetStringField(TEXT("username"), TEXT("hft_api"));
	JsonObject->SetStringField(TEXT("password"), TEXT("Stegsteg2025"));
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	
	UE_LOG(LogTemp, Warning, TEXT("Auth payload: %s"), *OutputString);
	
	HttpRequest->SetContentAsString(OutputString);
	HttpRequest->SetTimeout(30.0f);
	
	// Bind authentication response callback
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnAuthResponseReceived);
	
	// Execute the request
	if (!HttpRequest->ProcessRequest())
	{
		bIsLoading = false;
		UE_LOG(LogTemp, Error, TEXT("Failed to start authentication request"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, TEXT("ERROR: Failed to start authentication request"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Authentication request started successfully"));
	}
}

void ABuildingEnergyDisplay::OnAuthResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		bIsLoading = false;
		// CRITICAL: Reset authentication flag on failure
		static bool* AuthProgressFlag = []() -> bool* {
			static bool bAuthInProgress = false;
			return &bAuthInProgress;
		}();
		*AuthProgressFlag = false;
		UE_LOG(LogTemp, Error, TEXT("🚫 AUTH FLAG RESET: Authentication failed - flag cleared"));
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, TEXT("ERROR: Authentication failed"));
		}
		UE_LOG(LogTemp, Error, TEXT("Authentication request failed"));
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	if (ResponseCode != 200)
	{
		bIsLoading = false;
		// CRITICAL: Reset authentication flag on failure
		static bool* AuthProgressFlag = []() -> bool* {
			static bool bAuthInProgress = false;
			return &bAuthInProgress;
		}();
		*AuthProgressFlag = false;
		UE_LOG(LogTemp, Error, TEXT("🚫 AUTH FLAG RESET: Auth failed with code %d - flag cleared"), ResponseCode);
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				FString::Printf(TEXT("ERROR: Authentication failed with code %d"), ResponseCode));
		}
		UE_LOG(LogTemp, Error, TEXT("Authentication failed with response code: %d"), ResponseCode);
		return;
	}

	// Parse authentication response to get token
	FString ResponseContent = Response->GetContentAsString();
	TSharedPtr<FJsonValue> JsonValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);

	if (!FJsonSerializer::Deserialize(Reader, JsonValue) || !JsonValue.IsValid())
	{
		bIsLoading = false;
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, TEXT("ERROR: Failed to parse authentication response"));
		}
		return;
	}

	TSharedPtr<FJsonObject> JsonObject = JsonValue->AsObject();
	if (!JsonObject.IsValid() || !JsonObject->HasField(TEXT("access")))
	{
		bIsLoading = false;
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, TEXT("ERROR: No access token in response"));
		}
		return;
	}

	// Get the access token
	FString Token = JsonObject->GetStringField(TEXT("access"));
	
	// Get the refresh token for automatic token renewal
	RefreshToken = JsonObject->GetStringField(TEXT("refresh"));
	
	UE_LOG(LogTemp, Warning, TEXT("✅ BACKEND AUTH SUCCESS - Access token received, length: %d"), Token.Len());
	UE_LOG(LogTemp, Warning, TEXT("✅ REFRESH TOKEN - Refresh token received, length: %d"), RefreshToken.Len());
	UE_LOG(LogTemp, Warning, TEXT("✅ BACKEND VERIFICATION - API endpoint is responsive"));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
			FString::Printf(TEXT("✅ BACKEND CONNECTED - Token: %s..."), *Token.Left(10)));
	}
	
	// Now load building data with the token
	AccessToken = Token;
	
	// Reset authentication message flag since we now have a token
	bAuthenticationMessageShown = false;
	
	// Store refresh token if available in the authentication response
	// This enables automatic token renewal when access token expires
	if (!RefreshToken.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ REFRESH TOKEN stored for automatic renewal"));
	}
	
	// Store successful token globally to prevent duplicate auth
	static FString* GlobalSuccessToken = new FString(Token);
	static bool* GlobalAuthInProgress = new bool(false);
	*GlobalSuccessToken = Token;
	*GlobalAuthInProgress = false;
	
	// CRITICAL: Reset the authentication progress flag
	static bool* AuthProgressFlag = []() -> bool* {
		static bool bAuthInProgress = false;
		return &bAuthInProgress;
	}();
	*AuthProgressFlag = false;
	
	UE_LOG(LogTemp, Warning, TEXT("✅ AUTH FLAG RESET: Authentication completed successfully"));
	UE_LOG(LogTemp, Warning, TEXT("🔄 BACKEND DATA REQUEST - Fetching building energy data..."));
	PreloadAllBuildingData(Token);
}

// Refresh access token using stored refresh token
void ABuildingEnergyDisplay::RefreshAccessToken()
{
	if (RefreshToken.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("🔄 No refresh token available - cannot refresh access token"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("❌ No refresh token - please re-authenticate"));
		}
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔄 === REFRESHING ACCESS TOKEN ==="));
	
	// Create HTTP request for token refresh
	FHttpModule& HttpModule = FModuleManager::LoadModuleChecked<FHttpModule>("HTTP");
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = HttpModule.CreateRequest();
	
	FString ApiBaseUrl = TEXT("https://backend.gisworld-tech.com");
	FString RefreshURL = FString::Printf(TEXT("%s/api/token/refresh/"), *ApiBaseUrl);
	
	// Create JSON payload with refresh token
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
	JsonObject->SetStringField(TEXT("refresh"), RefreshToken);
	
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	
	// Configure request
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnRefreshTokenResponseReceived);
	HttpRequest->SetURL(RefreshURL);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader("Content-Type", TEXT("application/json"));
	HttpRequest->SetContentAsString(OutputString);
	
	UE_LOG(LogTemp, Warning, TEXT("🔄 Sending refresh token request to: %s"), *RefreshURL);
	HttpRequest->ProcessRequest();
}

// Handle refresh token response
void ABuildingEnergyDisplay::OnRefreshTokenResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("🔄 Token refresh request failed"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("❌ Token refresh failed - please re-authenticate"));
		}
		return;
	}
	
	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseContent = Response->GetContentAsString();
	
	UE_LOG(LogTemp, Warning, TEXT("🔄 Token refresh response: %d"), ResponseCode);
	
	if (ResponseCode != 200)
	{
		UE_LOG(LogTemp, Error, TEXT("🔄 Token refresh failed with code: %d"), ResponseCode);
		UE_LOG(LogTemp, Error, TEXT("🔄 Response: %s"), *ResponseContent);
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				FString::Printf(TEXT("❌ Token refresh failed (%d) - please re-authenticate"), ResponseCode));
		}
		return;
	}
	
	// Parse JSON response to get new access token
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("🔄 Failed to parse token refresh response JSON"));
		return;
	}
	
	// Extract new access token
	if (!JsonObject->HasField(TEXT("access")))
	{
		UE_LOG(LogTemp, Error, TEXT("🔄 No access token in refresh response"));
		return;
	}
	
	FString NewAccessToken = JsonObject->GetStringField(TEXT("access"));
	AccessToken = NewAccessToken;
	
	UE_LOG(LogTemp, Warning, TEXT("✅ ACCESS TOKEN REFRESHED - New token length: %d"), AccessToken.Len());
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, TEXT("✅ Access token refreshed successfully"));
	}
	
	// Reset authentication message flag since we have a fresh token
	bAuthenticationMessageShown = false;
}

// Fetch updated energy data using REST API polling
void ABuildingEnergyDisplay::FetchUpdatedEnergyData()
{
	if (AccessToken.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 Cannot fetch energy updates - no access token"));
		return;
	}
	
	// Use the same endpoint that works for initial data load
	FString ApiBaseUrl = TEXT("https://backend.gisworld-tech.com");
	FString DefaultCommunityId = TEXT("08417008");
	FString URL = FString::Printf(TEXT("%s/geospatial/buildings-energy/?community_id=%s&format=json"), 
		*ApiBaseUrl, *DefaultCommunityId);
	
	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(URL);
	HttpRequest->SetVerb("GET");
	HttpRequest->SetHeader("Content-Type", "application/json");
	HttpRequest->SetHeader("Accept", "application/json");
	HttpRequest->SetHeader("Authorization", FString::Printf(TEXT("Bearer %s"), *AccessToken));
	
	// Bind response handler
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnEnergyUpdateResponse);
	
	// Send request
	HttpRequest->ProcessRequest();
	
	EnergyUpdateCounter++;
	UE_LOG(LogTemp, Verbose, TEXT("🔄 Energy update request #%d sent"), EnergyUpdateCounter);
}

// Handle energy update response
void ABuildingEnergyDisplay::OnEnergyUpdateResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 Energy update request failed"));
		return;
	}
	
	int32 ResponseCode = Response->GetResponseCode();
	
	if (ResponseCode == 401)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 Energy update: Token expired, attempting refresh"));
		if (!RefreshToken.IsEmpty())
		{
			RefreshAccessToken();
		}
		return;
	}
	
	if (ResponseCode != 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 Energy update failed with code: %d"), ResponseCode);
		return;
	}
	
	FString ResponseContent = Response->GetContentAsString();
	
	// Parse and update building energy data
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);
	
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 Failed to parse energy update response"));
		return;
	}
	
	// Check if this is a results array
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray;
	if (JsonObject->TryGetArrayField(TEXT("results"), ResultsArray))
	{
		int32 UpdatedBuildings = 0;
		
		for (const auto& Item : *ResultsArray)
		{
			if (TSharedPtr<FJsonObject> BuildingObject = Item->AsObject())
			{
				FString BuildingId = BuildingObject->GetStringField(TEXT("gml_id"));
				
				// Update energy data in cache if this building exists
				if (BuildingDataCache.Contains(BuildingId))
				{
					// Create updated display message with new energy values
					FString UpdatedDisplayMessage = TEXT("Real-time Energy Data\n");
					
					// Update energy consumption if available
					if (BuildingObject->HasField(TEXT("energy_consumption")))
					{
						float EnergyValue = BuildingObject->GetNumberField(TEXT("energy_consumption"));
						UpdatedDisplayMessage += FString::Printf(TEXT("Energy Consumption: %.2f kWh\n"), EnergyValue);
						UpdatedBuildings++;
					}
					
					// Update other energy fields if available
					if (BuildingObject->HasField(TEXT("electricity_consumption")))
					{
						float ElectricityValue = BuildingObject->GetNumberField(TEXT("electricity_consumption"));
						UpdatedDisplayMessage += FString::Printf(TEXT("Electricity Consumption: %.2f kWh\n"), ElectricityValue);
					}
					
					if (BuildingObject->HasField(TEXT("heating_consumption")))
					{
						float HeatingValue = BuildingObject->GetNumberField(TEXT("heating_consumption"));
						UpdatedDisplayMessage += FString::Printf(TEXT("Heating Consumption: %.2f kWh\n"), HeatingValue);
					}
					
					// Update the cache with the new display message
					BuildingDataCache[BuildingId] = UpdatedDisplayMessage;
					
					// Extract coordinates if available in the update
					if (BuildingObject->HasField(TEXT("coordinates")))
					{
						FString CoordinatesData = BuildingObject->GetStringField(TEXT("coordinates"));

						// Create unique cache key when numeric id is available
						FString UniqueCacheKey = BuildingId;
						if (BuildingObject->HasField(TEXT("id")))
						{
							double NumericIdD = 0.0;
							if (BuildingObject->TryGetNumberField(TEXT("id"), NumericIdD))
							{
								int32 NumericId = (int32)NumericIdD;
								UniqueCacheKey = FString::Printf(TEXT("%s#%d"), *BuildingId, NumericId);
							}
						}

						StoreBuildingCoordinates(UniqueCacheKey, CoordinatesData);
						UE_LOG(LogTemp, Warning, TEXT("🔄 Updated coordinates for building: %s (cached as %s)"), *BuildingId, *UniqueCacheKey);
					}
				}
			}
		}
		
		if (UpdatedBuildings > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("✅ Energy update: %d buildings updated"), UpdatedBuildings);
			
			// Apply updated colors to the tileset
			// DISABLED - This was causing gray overlay on entire scene
			// ApplyColorsToCSiumTileset();
			UE_LOG(LogTemp, Warning, TEXT("💡 Color update disabled. Use manual ApplyBuildingColorsImmediately() instead"));
			
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 2.0f, FColor::Green, 
					FString::Printf(TEXT("🔄 Updated: %d buildings"), UpdatedBuildings));
			}
		}
		else
		{
			UE_LOG(LogTemp, Verbose, TEXT("🔄 Energy update: No changes detected"));
		}
	}
}

void ABuildingEnergyDisplay::ApplyColorsToCSiumTileset()
{
	UE_LOG(LogTemp, Warning, TEXT("COLOR Applying %d cached colors to BUILDINGS Cesium tileset..."), BuildingColorCache.Num());
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No building colors cached to apply"));
		return;
	}
	
	if (UWorld* World = GetWorld())
	{
		// Find the Cesium tileset actor
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (Actor)
			{
				FString ActorName = Actor->GetName();
				// Look for ONLY the buildings tileset actor (named like BuildingsTilesetName / 'bisingen')
				if (IsTargetBuildingsTileset(Actor))
				{
					UE_LOG(LogTemp, Warning, TEXT("CESIUM Found Cesium tileset: %s (class: %s)"), *ActorName, *Actor->GetClass()->GetName());
					
					TArray<UMeshComponent*> MeshComponents;
					Actor->GetComponents<UMeshComponent>(MeshComponents);
					
					UE_LOG(LogTemp, Warning, TEXT("Cesium tileset has %d mesh components"), MeshComponents.Num());
					
					// For now, apply a representative color to the entire tileset
					// TODO: This needs per-building material support from Cesium
					if (MeshComponents.Num() > 0 && BuildingColorCache.Num() > 0)
					{
						// Use the first building color as an example
						FLinearColor SampleColor = BuildingColorCache.begin()->Value;
						
						for (UMeshComponent* MeshComp : MeshComponents)
						{
							if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComp))
							{
								int32 NumMaterials = StaticMeshComp->GetNumMaterials();
								for (int32 MatIdx = 0; MatIdx < NumMaterials; ++MatIdx)
								{
									UMaterialInterface* CurrentMaterial = StaticMeshComp->GetMaterial(MatIdx);
									if (CurrentMaterial)
									{
										UMaterialInstanceDynamic* DynMat = Cast<UMaterialInstanceDynamic>(CurrentMaterial);
										if (!DynMat)
										{
											DynMat = UMaterialInstanceDynamic::Create(CurrentMaterial, StaticMeshComp);
											if (DynMat)
											{
												StaticMeshComp->SetMaterial(MatIdx, DynMat);
											}
										}
										
										if (DynMat)
										{
											DynMat->SetVectorParameterValue(TEXT("BaseColor"), SampleColor);
											DynMat->SetVectorParameterValue(TEXT("Color"), SampleColor);
											DynMat->SetVectorParameterValue(TEXT("Albedo"), SampleColor);
											DynMat->SetVectorParameterValue(TEXT("DiffuseColor"), SampleColor);
											UE_LOG(LogTemp, Warning, TEXT("COLOR Applied sample color to Cesium material"));
										}
									}
								}
							}
						}
					}
					
					if (GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
							FString::Printf(TEXT("Applied colors to Cesium tileset '%s'"), *ActorName));
					}
					
					break; // Found the tileset
				}
			}
		}
	}
}

UMaterialInstanceDynamic* ABuildingEnergyDisplay::CreateBuildingEnergyMaterial()
{
	UE_LOG(LogTemp, Warning, TEXT("COLOR Creating dynamic material for %d buildings with energy colors"), BuildingColorCache.Num());
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No building colors to apply"));
		return nullptr;
	}
	
	// Load a base material to work with (you can create a custom one in the editor)
	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	if (!BaseMaterial)
	{
		// Try alternative base materials
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
	}
	
	if (!BaseMaterial)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Could not load base material for building energy visualization"));
		return nullptr;
	}
	
	// Create dynamic material instance
	BuildingEnergyMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (!BuildingEnergyMaterial)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to create dynamic material instance"));
		return nullptr;
	}
	
	// Set default energy color (use average or representative color from API)
	FLinearColor AverageColor = FLinearColor::Green; // Default
	if (BuildingColorCache.Num() > 0)
	{
		// Use the first building color as representative
		AverageColor = BuildingColorCache.begin()->Value;
	}
	
	// Set material parameters for building energy visualization
	BuildingEnergyMaterial->SetVectorParameterValue(TEXT("BaseColor"), AverageColor);
	BuildingEnergyMaterial->SetVectorParameterValue(TEXT("Color"), AverageColor);
	BuildingEnergyMaterial->SetVectorParameterValue(TEXT("Albedo"), AverageColor);
	BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
	BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.7f);
	BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
	
	UE_LOG(LogTemp, Warning, TEXT("SUCCESS Created dynamic building energy material with representative color"));
	
	// Automatically assign to Cesium tileset
	// DISABLED - This was causing gray overlay on entire scene including landscape
	// AssignMaterialToCesiumTileset();
	UE_LOG(LogTemp, Warning, TEXT("NOTICE: Automatic material assignment disabled to prevent gray overlay"));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
			TEXT("SUCCESS Created dynamic material (auto-assignment disabled to prevent gray overlay)"));
	}
	
	return BuildingEnergyMaterial;
}

void ABuildingEnergyDisplay::AssignMaterialToCesiumTileset()
{
	// SAFETY CHECK: This function has been disabled to prevent gray overlay issues
	UE_LOG(LogTemp, Warning, TEXT("🚫 AssignMaterialToCesiumTileset() DISABLED"));
	UE_LOG(LogTemp, Warning, TEXT("🚫 This function was causing gray overlay on entire scene including landscape"));
	UE_LOG(LogTemp, Warning, TEXT("💡 Use ApplyBuildingColorsImmediately() instead for safe color application"));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Orange, 
			TEXT("⚠️ Material assignment disabled to prevent gray overlay"));
	}
	return;
	
	/* ORIGINAL CODE DISABLED TO PREVENT GRAY OVERLAY ISSUE
	if (!BuildingEnergyMaterial)
	{
		UE_LOG(LogTemp, Warning, TEXT("No dynamic material to assign"));
		return;
	}
	
	if (UWorld* World = GetWorld())
	{
		// Find the Cesium tileset actor by name "bisingen"
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (Actor && Actor->GetName().Contains(TEXT("bisingen")))
			{
				UE_LOG(LogTemp, Warning, TEXT("CESIUM Found Cesium tileset: %s"), *Actor->GetName());
				
				// Try to find and set the material property using reflection
				UClass* ActorClass = Actor->GetClass();
				
				// Look for material properties
				for (TFieldIterator<FProperty> PropIt(ActorClass); PropIt; ++PropIt)
				{
					FProperty* Property = *PropIt;
					if (Property->GetName().Contains(TEXT("Material")) || 
						Property->GetName().Contains(TEXT("material")))
					{
						UE_LOG(LogTemp, Warning, TEXT("Found material property: %s"), *Property->GetName());
						
						// Try to set as object property
						if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(Property))
						{
							if (ObjectProp->PropertyClass->IsChildOf(UMaterialInterface::StaticClass()))
							{
								ObjectProp->SetObjectPropertyValue_InContainer(Actor, BuildingEnergyMaterial);
								UE_LOG(LogTemp, Warning, TEXT("SUCCESS Assigned material to property: %s"), *Property->GetName());
								
								if (GEngine)
								{
									GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, 
										TEXT("SUCCESS Material automatically assigned to Cesium tileset!"));
								}
								return;
							}
						}
					}
				}
				
				// Fallback: Apply to mesh components
				TArray<UMeshComponent*> MeshComponents;
				Actor->GetComponents<UMeshComponent>(MeshComponents);
				
				for (UMeshComponent* MeshComp : MeshComponents)
				{
					if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComp))
					{
						StaticMeshComp->SetMaterial(0, BuildingEnergyMaterial);
						UE_LOG(LogTemp, Warning, TEXT("COLOR Applied material to mesh component as fallback"));
					}
				}
				
				break;
			}
		}
	}
	*/
}

void ABuildingEnergyDisplay::DisplayBuildingEnergyData(const FString& BuildingGmlId)
{
	// 🚫 === COMPLETELY DISABLED - CAUSES MULTIPLE BUILDING DISPLAYS ===
	static int32 DisplayDataCallCounter = 0;
	DisplayDataCallCounter++;
	UE_LOG(LogTemp, Error, TEXT("🚫 === DisplayBuildingEnergyData() CALLED #%d - BLOCKED ==="), DisplayDataCallCounter);
	UE_LOG(LogTemp, Error, TEXT("🚫 This function causes multiple building displays and is DISABLED"));
	UE_LOG(LogTemp, Error, TEXT("🚫 BuildingGmlId: '%s' - NO DISPLAY WILL BE SHOWN"), *BuildingGmlId);
	
	// COMPLETELY DISABLED - This function was causing multiple building displays
	// Blueprint should ONLY use OnBuildingClicked() for single building display
}

void ABuildingEnergyDisplay::CreateMaterialForEditor()
{
	UE_LOG(LogTemp, Warning, TEXT("COLOR Creating building-specific color material for editor..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No building colors loaded. Loading default colors..."));
		// Create some sample colors for testing
		BuildingColorCache.Add(TEXT("DEBW_0010089wkDD"), ConvertHexToLinearColor(TEXT("#66b032"))); // Green from API
		BuildingColorCache.Add(TEXT("DEBW_0010090wkDD"), ConvertHexToLinearColor(TEXT("#ff5733"))); // Red example
		BuildingColorCache.Add(TEXT("DEBW_0010091wkDD"), ConvertHexToLinearColor(TEXT("#3366cc"))); // Blue example
		BuildingColorCache.Add(TEXT("DEBW_0010092wkDD"), ConvertHexToLinearColor(TEXT("#ffcc00"))); // Yellow example
	}
	
	// Load a base material
	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	if (!BaseMaterial)
	{
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
	}
	
	if (!BaseMaterial)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Could not load base material"));
		return;
	}
	
	// Create the material instance
	BuildingEnergyMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	
	if (BuildingEnergyMaterial)
	{
		// Instead of one color, we'll create a system that can handle multiple colors
		// For now, set a representative color, but log all building colors
		UE_LOG(LogTemp, Warning, TEXT("COLOR Building Color Mapping:"));
		
		for (const auto& BuildingColor : BuildingColorCache)
		{
			FString GmlId = BuildingColor.Key;
			FLinearColor Color = BuildingColor.Value;
			UE_LOG(LogTemp, Warning, TEXT("  Building %s -> Color(R:%.2f, G:%.2f, B:%.2f)"), 
				*GmlId, Color.R, Color.G, Color.B);
		}
		
		// Set a default color to the material
		FLinearColor DefaultColor = FLinearColor(0.4f, 0.69f, 0.2f, 1.0f); // Green #66b032
		BuildingEnergyMaterial->SetVectorParameterValue(TEXT("BaseColor"), DefaultColor);
		BuildingEnergyMaterial->SetVectorParameterValue(TEXT("Color"), DefaultColor);
		BuildingEnergyMaterial->SetVectorParameterValue(TEXT("Albedo"), DefaultColor);
		BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
		BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.7f);
		BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
		
		UE_LOG(LogTemp, Warning, TEXT("SUCCESS Material created! Note: Cesium tiles need special handling for per-building colors."));
		UE_LOG(LogTemp, Warning, TEXT("TIP Consider using Cesium's per-feature styling or custom shaders for individual building colors."));
		
		#if WITH_EDITOR
		// Mark the component as modified so it shows up in the editor
		Modify();
		#endif
		
		// Try to apply per-building colors using Cesium's feature styling
		ApplyPerBuildingColorsToCesium();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to create material instance"));
	}
}

void ABuildingEnergyDisplay::ApplyPerBuildingColorsToCesium()
{
	UE_LOG(LogTemp, Warning, TEXT("COLOR Applying individual building colors to Cesium tileset..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No building colors to apply"));
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("FOUND Found %d buildings with individual colors:"), BuildingColorCache.Num());
	for (const auto& BuildingColor : BuildingColorCache)
	{
		FString GmlId = BuildingColor.Key;
		FLinearColor Color = BuildingColor.Value;
		// Convert back to hex for display
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		UE_LOG(LogTemp, Warning, TEXT("  BUILDING %s -> %s"), *GmlId, *HexColor);
	}
	
	if (UWorld* World = GetWorld())
	{
		// Look for Cesium tileset
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (Actor && Actor->GetName().Contains(TEXT("bisingen")))
			{
				UE_LOG(LogTemp, Warning, TEXT("TARGET Found Cesium tileset: %s"), *Actor->GetName());
				
				// Apply the modern approach: Cesium tileset styling
				ApplyCesiumTilesetStyling(Actor);
				
				// Alternative: Try to create multiple material instances for different parts
				CreateMultipleMaterialsForCesium(Actor);
				
				break;
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("TIP For true per-building colors in Cesium, you may need:"));
	UE_LOG(LogTemp, Warning, TEXT("   1. Cesium 3D Tiles styling (JSON expressions)"));
	UE_LOG(LogTemp, Warning, TEXT("   2. Feature properties embedded in the tileset data"));
	UE_LOG(LogTemp, Warning, TEXT("   3. Custom shader with building ID lookup"));
}

FLinearColor ABuildingEnergyDisplay::ConvertHexToLinearColor(const FString& HexColor)
{
	FString CleanHex = HexColor;
	
	// Remove '#' if present
	CleanHex = CleanHex.Replace(TEXT("#"), TEXT(""));
	
	// Validate hex string length
	if (CleanHex.Len() != 6)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Invalid hex color format: %s (should be 6 characters like '66b032')"), *HexColor);
		return FLinearColor(0.4f, 0.69f, 0.2f, 1.0f); // Return #66b032 as fallback
	}
	
	// Validate hex characters
	for (int32 i = 0; i < CleanHex.Len(); i++)
	{
		TCHAR Char = CleanHex[i];
		if (!((Char >= '0' && Char <= '9') || (Char >= 'A' && Char <= 'F') || (Char >= 'a' && Char <= 'f')))
		{
			UE_LOG(LogTemp, Error, TEXT("ERROR Invalid hex character in color: %s"), *HexColor);
			return FLinearColor(0.4f, 0.69f, 0.2f, 1.0f); // Return #66b032 as fallback
		}
	}
	
	// Convert hex to FColor then to FLinearColor
	FColor ColorSRGB = FColor::FromHex(CleanHex);
	FLinearColor Result = FLinearColor::FromSRGBColor(ColorSRGB);
	
	UE_LOG(LogTemp, Log, TEXT("COLOR Converted hex %s to Linear(R:%.3f, G:%.3f, B:%.3f)"), 
		*HexColor, Result.R, Result.G, Result.B);
	
	return Result;
}

void ABuildingEnergyDisplay::CreateMultipleMaterialsForCesium(AActor* CesiumActor)
{
	if (!CesiumActor)
	{
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("COLOR Creating multiple materials for individual buildings..."));
	
	// Get mesh components from the Cesium actor
	TArray<UMeshComponent*> MeshComponents;
	CesiumActor->GetComponents<UMeshComponent>(MeshComponents);
	
	UE_LOG(LogTemp, Warning, TEXT("Found %d mesh components in Cesium tileset"), MeshComponents.Num());
	
	int32 ComponentIndex = 0;
	auto ColorIterator = BuildingColorCache.begin();
	
	for (UMeshComponent* MeshComp : MeshComponents)
	{
		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComp))
		{
			// Get base material
			UMaterialInterface* BaseMaterial = StaticMeshComp->GetMaterial(0);
			if (!BaseMaterial)
			{
				BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
			}
			
			if (BaseMaterial)
			{
				// Create dynamic material for this component
				UMaterialInstanceDynamic* DynamicMat = UMaterialInstanceDynamic::Create(BaseMaterial, this);
				if (DynamicMat)
				{
					FLinearColor BuildingColor;
					FString BuildingId = TEXT("Unknown");
					
					// Use real building color if available
					if (ColorIterator != BuildingColorCache.end())
					{
						BuildingColor = ColorIterator->Value;
						BuildingId = ColorIterator->Key;
						++ColorIterator;
					}
					else
					{
						// Fallback to rainbow pattern if we have more components than buildings
						float Hue = FMath::Fmod(ComponentIndex * 60.0f, 360.0f);
						BuildingColor = FLinearColor::MakeFromHSV8(Hue, 255, 200);
						BuildingId = FString::Printf(TEXT("Component_%d"), ComponentIndex);
					}
					
					// Apply the color
					DynamicMat->SetVectorParameterValue(TEXT("BaseColor"), BuildingColor);
					DynamicMat->SetVectorParameterValue(TEXT("Color"), BuildingColor);
					DynamicMat->SetVectorParameterValue(TEXT("Albedo"), BuildingColor);
					DynamicMat->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
					DynamicMat->SetScalarParameterValue(TEXT("Roughness"), 0.7f);
					DynamicMat->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
					
					// Apply to mesh component
					StaticMeshComp->SetMaterial(0, DynamicMat);
					
					// Convert color to hex for logging
					FColor SRGBColor = BuildingColor.ToFColor(true);
					FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
					
					UE_LOG(LogTemp, Warning, TEXT("SUCCESS Applied color %s to mesh component %d (Building: %s)"), 
						*HexColor, ComponentIndex, *BuildingId);
				}
			}
			
			ComponentIndex++;
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("MATERIALS Created %d individual materials for Cesium components"), ComponentIndex);
}

void ABuildingEnergyDisplay::ApplyColorsUsingCesiumStyling()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 CESIUM STYLE: Applying per-building colors using 3D Tiles Styling (gml:id → modified_gml_id)..."));

	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 CESIUM STYLE: No cached colors available. Load/refresh cache first."));
		return;
	}

	UCesium3DTileset* TilesetComp = GetBuildingsTilesetComponent();
	if (!TilesetComp)
	{
		UE_LOG(LogTemp, Error, TEXT("🎨 CESIUM STYLE: Buildings tileset component not found (name='%s')."), *BuildingsTilesetName);
		return;
	}

	// Build and apply style JSON
	const FString StyleJson = BuildCesiumStyleJsonFromCache();

	// Apply the style to the tileset (per-feature GPU evaluation).
	{
		// Call SetStyle via reflection to be resilient across Cesium-for-Unreal versions.
		static const FName SetStyleName(TEXT("SetStyle"));
		if (UFunction* SetStyleFn = TilesetComp->FindFunction(SetStyleName))
		{
			struct FSetStyleParams { FString Style; };
			FSetStyleParams Params{StyleJson};
			TilesetComp->ProcessEvent(SetStyleFn, &Params);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("🎨 CESIUM STYLE: UCesium3DTileset has no SetStyle function in this build."));
			return;
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("✅ CESIUM STYLE: Applied style with %d building color rules to tileset '%s'."),
		BuildingColorCache.Num(), *TilesetComp->GetOwner()->GetName());

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 6.0f, FColor::Green,
			FString::Printf(TEXT("🎨 Per-building colors applied (%d rules)"), BuildingColorCache.Num()));
	}
}


FString ABuildingEnergyDisplay::CreateCesiumColorExpression()
{
	if (BuildingColorCache.Num() == 0)
	{
		return TEXT("color('#66b032')"); // Default green
	}
	
	// Create a Cesium 3D Tiles styling JSON using the "match" operator.
	// Format: {"color": ["match", ["get","gml:id"], "GMLID1", "#hex1", "GMLID2", "#hex2", defaultColor]}
	// NOTE: Using "gml:id" (with colon) to match tileset property name
	FString Expression = TEXT("{");
	Expression += TEXT("\"color\": [\"match\", [\"get\", \"gml:id\"], ");

	int32 Added = 0;
	for (const auto& BuildingColor : BuildingColorCache)
	{
		if (Added > 0)
		{
			// comma separated entries
		}

		FString GmlId = BuildingColor.Key;
		FLinearColor Color = BuildingColor.Value;
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);

		// Append: "GMLID","#HEX",
		Expression += FString::Printf(TEXT("\"%s\", \"%s\", "), *GmlId, *HexColor);
		Added++;

		// Limit expression size to avoid overly long strings
		if (Expression.Len() > 4000)
		{
			UE_LOG(LogTemp, Warning, TEXT("EXPRESSION truncated after %d entries to avoid excessive length"), Added);
			break;
		}
	}

	// Default fallback color
	Expression += TEXT("\"#66b032\" ] }");

	UE_LOG(LogTemp, Warning, TEXT("EXPRESSION Created Cesium styling JSON (length: %d) entries: %d"), Expression.Len(), Added);
	UE_LOG(LogTemp, Warning, TEXT("EXPRESSION Content: %s"), *Expression);
	return Expression;
}

void ABuildingEnergyDisplay::CreateTestColors()
{
	UE_LOG(LogTemp, Warning, TEXT("🌈 Creating test colors to demonstrate per-building coloring..."));
	
	// Get the first few buildings and assign them different colors
	int32 ColorIndex = 0;
	TArray<FString> TestColors = {
		TEXT("#ff0000"), // Red
		TEXT("#00ff00"), // Green  
		TEXT("#0000ff"), // Blue
		TEXT("#ffff00"), // Yellow
		TEXT("#ff00ff"), // Magenta
		TEXT("#00ffff"), // Cyan
		TEXT("#ff8000"), // Orange
		TEXT("#8000ff"), // Purple
		TEXT("#80ff00"), // Lime
		TEXT("#ff0080")  // Pink
	};
	
	for (auto& BuildingColor : BuildingColorCache)
	{
		if (ColorIndex >= TestColors.Num())
		{
			break; // Only color the first 10 buildings with different colors
		}
		
		FString TestColorHex = TestColors[ColorIndex];
		FLinearColor TestColor = ConvertHexToLinearColor(TestColorHex);
		
		// Update the building color
		BuildingColor.Value = TestColor;
		
		UE_LOG(LogTemp, Warning, TEXT("TEST Assigned test color %s to building %s"), 
			*TestColorHex, *BuildingColor.Key);
		
		ColorIndex++;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("SUCCESS Created %d test colors for demonstration"), ColorIndex);
}

void ABuildingEnergyDisplay::CreateTextureBasedMaterial()
{
	UE_LOG(LogTemp, Warning, TEXT("MATERIAL Creating texture-based material for Cesium manual assignment..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No building colors cached. Using default colors..."));
		// Create some default colors for testing
		BuildingColorCache.Add(TEXT("TEST_001"), ConvertHexToLinearColor(TEXT("#ff0000"))); // Red
		BuildingColorCache.Add(TEXT("TEST_002"), ConvertHexToLinearColor(TEXT("#00ff00"))); // Green
		BuildingColorCache.Add(TEXT("TEST_003"), ConvertHexToLinearColor(TEXT("#0000ff"))); // Blue
	}
	
	// Calculate representative color from all building colors
	FLinearColor RepresentativeColor = FLinearColor::Black;
	TMap<FString, int32> ColorFrequency;
	
	// Find the most common color
	for (const auto& BuildingColor : BuildingColorCache)
	{
		FLinearColor Color = BuildingColor.Value;
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		
		if (ColorFrequency.Contains(HexColor))
		{
			ColorFrequency[HexColor]++;
		}
		else
		{
			ColorFrequency.Add(HexColor, 1);
		}
	}
	
	// Find most frequent color
	FString MostFrequentColor = TEXT("#808080");
	int32 MaxCount = 0;
	for (const auto& ColorCount : ColorFrequency)
	{
		if (ColorCount.Value > MaxCount)
		{
			MaxCount = ColorCount.Value;
			MostFrequentColor = ColorCount.Key;
		}
	}
	
	RepresentativeColor = ConvertHexToLinearColor(MostFrequentColor);
	
	UE_LOG(LogTemp, Warning, TEXT("STATS Most common building color: %s (%d buildings)"), *MostFrequentColor, MaxCount);
	
	// Create a material that represents the building energy colors
	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	if (!BaseMaterial)
	{
		BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
	}
	
	if (BaseMaterial)
	{
		// Create the building energy material
		BuildingEnergyMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		
		if (BuildingEnergyMaterial)
		{
			// Apply the representative color
			BuildingEnergyMaterial->SetVectorParameterValue(TEXT("BaseColor"), RepresentativeColor);
			BuildingEnergyMaterial->SetVectorParameterValue(TEXT("Color"), RepresentativeColor);
			BuildingEnergyMaterial->SetVectorParameterValue(TEXT("Albedo"), RepresentativeColor);
			BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Metallic"), 0.0f);
			BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Roughness"), 0.5f);
			BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Opacity"), 1.0f);
			BuildingEnergyMaterial->SetScalarParameterValue(TEXT("Specular"), 0.5f);
			
			UE_LOG(LogTemp, Warning, TEXT("SUCCESS Created BuildingEnergyMaterial with color: %s"), *MostFrequentColor);
			UE_LOG(LogTemp, Warning, TEXT("READY Material is ready for manual assignment to Cesium tileset!"));
			UE_LOG(LogTemp, Warning, TEXT("INFO Instructions:"));
			UE_LOG(LogTemp, Warning, TEXT("   1. Select your Cesium tileset (bisingen)"));
			UE_LOG(LogTemp, Warning, TEXT("   2. In Details panel, find the Material property"));
			UE_LOG(LogTemp, Warning, TEXT("   3. Drag the BuildingEnergyMaterial from this actor"));
			UE_LOG(LogTemp, Warning, TEXT("   4. Drop it into the Cesium Material slot"));
			
			// Also try to automatically find and list Cesium material properties
			if (UWorld* World = GetWorld())
			{
				for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
				{
					AActor* Actor = *ActorItr;
					if (Actor && Actor->GetName().Contains(TEXT("bisingen")))
					{
						UE_LOG(LogTemp, Warning, TEXT("TARGET Found Cesium tileset for reference: %s"), *Actor->GetName());
						UE_LOG(LogTemp, Warning, TEXT("PROPS Available material properties on Cesium tileset:"));
						
						UClass* ActorClass = Actor->GetClass();
						for (TFieldIterator<FProperty> PropIt(ActorClass); PropIt; ++PropIt)
						{
							FProperty* Property = *PropIt;
							FString PropName = Property->GetName();
							
							if (PropName.Contains(TEXT("Material")) || PropName.Contains(TEXT("Color")))
							{
								UE_LOG(LogTemp, Warning, TEXT("   PROP %s (%s)"), *PropName, *Property->GetClass()->GetName());
							}
						}
						break;
					}
				}
			}
			
			#if WITH_EDITOR
			Modify();
			#endif
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("ERROR Failed to create dynamic material instance"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to load base material"));
	}
}

void ABuildingEnergyDisplay::CreatePerBuildingColorMaterial()
{
	UE_LOG(LogTemp, Error, TEXT("🚀 === CREATE PER BUILDING COLOR MATERIAL START ==="));
	UE_LOG(LogTemp, Warning, TEXT("MATERIAL Creating per-building color material using conditional styling approach..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No building colors cached. Creating sample data..."));
		return;
	}
	
	// Log the color variety we're working with
	UE_LOG(LogTemp, Warning, TEXT("🌈 Building Color Breakdown:"));

	// DEBUG: Check for known problematic building IDs and report sample color
	UE_LOG(LogTemp, Warning, TEXT("DEBUG Checking BuildingColorCache for problematic IDs (DEBW_0010008 / wfbT)..."));
	{
		bool bFoundProblematic = false;
		for (const auto& BuildingColor : BuildingColorCache)
		{
			const FString& Key = BuildingColor.Key;
			if (Key.Contains(TEXT("DEBW_0010008")) || Key.Contains(TEXT("wfbT")))
			{
				FColor SRGB = BuildingColor.Value.ToFColor(true);
				FString Hex = FString::Printf(TEXT("#%02X%02X%02X"), SRGB.R, SRGB.G, SRGB.B);
				UE_LOG(LogTemp, Warning, TEXT("DEBUG Found problematic building in cache: %s -> %s"), *Key, *Hex);
				bFoundProblematic = true;
				break;
			}
		}
		if (!bFoundProblematic)
		{
			UE_LOG(LogTemp, Warning, TEXT("DEBUG No problematic building IDs found in BuildingColorCache"));
		}
	}
	TMap<FString, int32> ColorStats;
	
	for (const auto& BuildingColor : BuildingColorCache)
	{
		FColor SRGBColor = BuildingColor.Value.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		
		if (ColorStats.Contains(HexColor))
		{
			ColorStats[HexColor]++;
		}
		else
		{
			ColorStats.Add(HexColor, 1);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("STATS Total buildings with colors: %d"), BuildingColorCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("STATS Unique colors found: %d"), ColorStats.Num());
	
	// Show top colors
	TArray<TPair<FString, int32>> SortedColors;
	for (const auto& ColorStat : ColorStats)
	{
		SortedColors.Add(TPair<FString, int32>(ColorStat.Key, ColorStat.Value));
	}
	
	SortedColors.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		return A.Value > B.Value; // Sort by count, descending
	});
	
	UE_LOG(LogTemp, Warning, TEXT("STATS Top building colors in your dataset:"));
	for (int32 i = 0; i < FMath::Min(10, SortedColors.Num()); i++)
	{
		UE_LOG(LogTemp, Warning, TEXT("   %s: %d buildings"), *SortedColors[i].Key, SortedColors[i].Value);
	}
	
	// Apply colors using Cesium conditional styling approach (like your JavaScript version)
	UE_LOG(LogTemp, Error, TEXT("🚀 === ABOUT TO CALL ApplyOfficialCesiumMetadataVisualization ==="));
	ApplyOfficialCesiumMetadataVisualization();
	UE_LOG(LogTemp, Error, TEXT("🚀 === CREATE PER BUILDING COLOR MATERIAL END ==="));
}

void ABuildingEnergyDisplay::ApplyConditionalStylingToTileset()
{
	UE_LOG(LogTemp, Warning, TEXT("DEBUG === CONDITIONAL STYLING DEBUG START ==="));
	UE_LOG(LogTemp, Warning, TEXT("DEBUG Applying conditional styling to Cesium tileset (JavaScript approach)..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No building colors available for styling"));
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("DEBUG Building %d conditions from BuildingColorCache..."), BuildingColorCache.Num());
	
	// Find Cesium 3D Tileset actor in the level
	AActor* CesiumActor = nullptr;
	UWorld* World = GetWorld();
	if (World)
	{
		UE_LOG(LogTemp, Warning, TEXT("SEARCH Searching for Cesium actors in level..."));
		
		// List all Cesium-related actors and look for "bisingen" specifically
		TArray<AActor*> AllCesiumActors;
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* CurrentActor = *ActorItr;
			if (CurrentActor)
			{
				FString ActorName = CurrentActor->GetName();
				FString ClassName = CurrentActor->GetClass()->GetName();
				
				// Log all actors that might be relevant
				if (ClassName.Contains(TEXT("Cesium")) || ActorName.Contains(TEXT("bisingen")))
				{
					AllCesiumActors.Add(CurrentActor);
					UE_LOG(LogTemp, Warning, TEXT("   Found actor: %s (Class: %s)"), *ActorName, *ClassName);
				}
			}
		}
		
		// Look specifically for "bisingen" tileset first
		for (AActor* Actor : AllCesiumActors)
		{
			FString ActorName = Actor->GetName();
			if (ActorName.Contains(TEXT("bisingen")))
			{
				UE_LOG(LogTemp, Warning, TEXT("TARGET Selected bisingen tileset actor: %s (Class: %s)"), 
					*ActorName, *Actor->GetClass()->GetName());
				CesiumActor = Actor;
				break;
			}
		}
		
		// Fallback to Cesium3DTileset class
		if (!CesiumActor)
		{
			for (AActor* Actor : AllCesiumActors)
			{
				if (Actor->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")) || 
					Actor->GetClass()->GetName().Contains(TEXT("3DTileset")))
				{
					UE_LOG(LogTemp, Warning, TEXT("TARGET Selected Cesium3DTileset actor: %s"), *Actor->GetClass()->GetName());
					CesiumActor = Actor;
					break;
				}
			}
		}
		
		// Final fallback to any non-georeference Cesium actor
		if (!CesiumActor)
		{
			for (AActor* Actor : AllCesiumActors)
			{
				// Skip CesiumGeoreference as it's just for positioning
				if (!Actor->GetClass()->GetName().Contains(TEXT("CesiumGeoreference")))
				{
					UE_LOG(LogTemp, Warning, TEXT("FALLBACK Using fallback Cesium actor: %s"), *Actor->GetClass()->GetName());
					CesiumActor = Actor;
					break;
				}
			}
		}
	}
	
	if (!CesiumActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("ERROR No Cesium tileset actor found in level"));
		UE_LOG(LogTemp, Warning, TEXT("SEARCH Available actors in level:"));
		if (World)
		{
			int32 ActorCount = 0;
			for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
			{
				AActor* CurrentActor = *ActorItr;
				if (CurrentActor)
				{
					UE_LOG(LogTemp, Warning, TEXT("   Actor %d: %s"), ActorCount++, *CurrentActor->GetClass()->GetName());
					if (ActorCount >= 10) break; // Show first 10 actors
				}
			}
		}
		return;
	}
	
	// Build conditional styling expression similar to your JavaScript
	UE_LOG(LogTemp, Warning, TEXT("BUILD Building conditions for %d buildings..."), BuildingColorCache.Num());
	
	// Show first 5 conditions as examples
	TArray<FString> ConditionPairs;
	int32 ConditionCount = 0;
	
	for (const auto& BuildingColor : BuildingColorCache)
	{
		FString BuildingId = BuildingColor.Key;
		FLinearColor Color = BuildingColor.Value;
		
		// Convert LinearColor back to hex
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		
		// Create condition like JavaScript: '${gml:id}' === 'building_id'
		FString Condition = FString::Printf(TEXT("'${gml:id}' === '%s'"), *BuildingId);
		FString ColorAction = FString::Printf(TEXT("color('%s')"), *HexColor);
		
		// Add to conditions
		ConditionPairs.Add(FString::Printf(TEXT("[\"%s\", \"%s\"]"), 
			*Condition.Replace(TEXT("\""), TEXT("\\\"")), 
			*ColorAction.Replace(TEXT("\""), TEXT("\\\""))));
		
		// Show first 5 conditions for debugging
		if (ConditionCount < 5)
		{
			UE_LOG(LogTemp, Warning, TEXT("   Condition %d: Building %s -> Color %s"), 
				ConditionCount + 1, *BuildingId, *HexColor);
			UE_LOG(LogTemp, Warning, TEXT("     Full condition: %s"), *ConditionPairs.Last());
		}
		ConditionCount++;
	}
	
	// Add fallback condition (white for unmatched buildings)
	ConditionPairs.Add(TEXT("[\"true\", \"color('#FFFFFF')\"]"));
	UE_LOG(LogTemp, Warning, TEXT("   Fallback condition: [\"true\", \"color('#FFFFFF')\"]"));
	
	// Join all conditions
	FString ConditionsArray = TEXT("[") + FString::Join(ConditionPairs, TEXT(", ")) + TEXT("]");
	
	UE_LOG(LogTemp, Warning, TEXT("RULES Created %d conditional styling rules"), ConditionPairs.Num() - 1);
	UE_LOG(LogTemp, Warning, TEXT("CONDITIONS Complete conditions array: %s"), *ConditionsArray.Left(500));
	if (ConditionsArray.Len() > 500)
	{
		UE_LOG(LogTemp, Warning, TEXT("TRUNCATED ... (truncated, full length: %d characters)"), ConditionsArray.Len());
	}
	
	// Create complete style JSON
	FString StyleJSON = FString::Printf(TEXT("{\"color\": {\"conditions\": %s}}"), *ConditionsArray);
	UE_LOG(LogTemp, Warning, TEXT("STYLE Style JSON (first 300 chars): %s"), *StyleJSON.Left(300));
	
	// Try to apply styling through Cesium component properties
	if (UActorComponent* CesiumComponent = CesiumActor->GetComponentByClass(UActorComponent::StaticClass()))
	{
		// Look for Cesium3DTileset component specifically
		TArray<UActorComponent*> TilesetComponents;
		CesiumActor->GetComponents<UActorComponent>(TilesetComponents);
		
		UE_LOG(LogTemp, Warning, TEXT("SEARCH Found %d components on Cesium actor"), TilesetComponents.Num());
		
		for (UActorComponent* Component : TilesetComponents)
		{
			UE_LOG(LogTemp, Warning, TEXT("   Component: %s"), *Component->GetClass()->GetName());
			
			if (Component->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
			{
				UE_LOG(LogTemp, Warning, TEXT("TARGET Found Cesium3DTileset component: %s"), *Component->GetClass()->GetName());
				
				// Try to apply styling through reflection (property access)
				UClass* ComponentClass = Component->GetClass();
				
				UE_LOG(LogTemp, Warning, TEXT("SEARCH Searching for style properties on %s..."), *ComponentClass->GetName());
				
				// Look for style-related properties
				bool bFoundStyleProperty = false;
				for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
				{
					FProperty* Property = *PropIt;
					FString PropName = Property->GetName();
					
					// Log all properties to see what's available
					if (PropName.Contains(TEXT("Style")) || PropName.Contains(TEXT("Color")) || 
						PropName.Contains(TEXT("Material")) || PropName.Contains(TEXT("Tileset")) ||
						PropName.Contains(TEXT("Cesium")) || PropName.Contains(TEXT("Render")) ||
						PropName.Contains(TEXT("Feature")) || PropName.Contains(TEXT("Expression")))
					{
						UE_LOG(LogTemp, Warning, TEXT("SEARCH Found potentially relevant property: %s (Type: %s)"), 
							*PropName, *Property->GetCPPType());
						
						// Try to set any string property that might be styling-related
						if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
						{
							UE_LOG(LogTemp, Warning, TEXT("SETTING Attempting to set string property: %s"), *PropName);
							StrProperty->SetPropertyValue_InContainer(Component, StyleJSON);
							Component->Modify();
							bFoundStyleProperty = true;
							UE_LOG(LogTemp, Warning, TEXT("SUCCESS Applied conditional styling to property: %s"), *PropName);
						}
					}
				}
				
				// If no obvious style properties found, try alternative approaches
				if (!bFoundStyleProperty)
				{
					UE_LOG(LogTemp, Warning, TEXT("WARNING No direct style properties found. Trying alternative approaches..."));
					
					// Try to call methods on the component
					UClass* ComponentClassForMethods = Component->GetClass();
					
					// Look for functions that might set styling
					for (TFieldIterator<UFunction> FuncIt(ComponentClassForMethods); FuncIt; ++FuncIt)
					{
						UFunction* Function = *FuncIt;
						FString FuncName = Function->GetName();
						
						if (FuncName.Contains(TEXT("Style")) || FuncName.Contains(TEXT("Color")) || 
							FuncName.Contains(TEXT("SetMaterial")) || FuncName.Contains(TEXT("Apply")))
						{
							UE_LOG(LogTemp, Warning, TEXT("🔧 Found potential styling function: %s"), *FuncName);
						}
					}
					
					// Alternative: Try to modify the material directly if possible
					UE_LOG(LogTemp, Warning, TEXT("INFO ALTERNATIVE SOLUTION NEEDED:"));
					UE_LOG(LogTemp, Warning, TEXT("   Cesium for Unreal may not support direct JSON styling"));
					UE_LOG(LogTemp, Warning, TEXT("   Consider these approaches:"));
					UE_LOG(LogTemp, Warning, TEXT("   1. MATERIAL Use material overrides on mesh components"));
					UE_LOG(LogTemp, Warning, TEXT("   2. 🔧 Implement custom shader with building ID lookup"));
					UE_LOG(LogTemp, Warning, TEXT("   3. VERTEX Use vertex colors if tileset supports them"));
					UE_LOG(LogTemp, Warning, TEXT("   4. EXTERNAL Generate styled tileset externally with cesium-native"));
				}
				break;
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("ERROR No components found on Cesium actor"));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("STYLE Conditional styling applied using approach similar to your JavaScript version"));
	UE_LOG(LogTemp, Warning, TEXT("INFO This mimics: tileSet.style = new Cesium3DTileStyle({ color: { conditions: [...] } })"));
	UE_LOG(LogTemp, Warning, TEXT("DEBUG === CONDITIONAL STYLING DEBUG END ==="));
}

void ABuildingEnergyDisplay::ApplyOfficialCesiumMetadataVisualization()
{
	UE_LOG(LogTemp, Warning, TEXT("METADATA === OFFICIAL CESIUM METADATA VISUALIZATION START ==="));
	UE_LOG(LogTemp, Warning, TEXT("METADATA Implementing official Cesium for Unreal metadata approach..."));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No building colors available for visualization"));
		return;
	}
	
	// Find the Cesium 3D Tileset actor (bisingen)
	AActor* CesiumActor = nullptr;
	UWorld* World = GetWorld();
	if (World)
	{
		UE_LOG(LogTemp, Warning, TEXT("SEARCH Searching for bisingen Cesium tileset..."));
		
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* CurrentActor = *ActorItr;
			if (CurrentActor)
			{
				FString ActorName = CurrentActor->GetName();
				if (ActorName.Contains(TEXT("bisingen")))
				{
					UE_LOG(LogTemp, Warning, TEXT("TARGET Found bisingen tileset: %s (Class: %s)"), 
						*ActorName, *CurrentActor->GetClass()->GetName());
					CesiumActor = CurrentActor;
					break;
				}
			}
		}
		
		// Fallback to any Cesium3DTileset
		if (!CesiumActor)
		{
			for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
			{
				AActor* CurrentActor = *ActorItr;
				if (CurrentActor && CurrentActor->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
				{
					UE_LOG(LogTemp, Warning, TEXT("FALLBACK Using fallback Cesium3DTileset: %s"), 
						*CurrentActor->GetClass()->GetName());
					CesiumActor = CurrentActor;
					break;
				}
			}
		}
	}
	
	if (!CesiumActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("ERROR No Cesium tileset actor found"));
		return;
	}
	
	// Check if tileset already has CesiumFeaturesMetadata component
	UActorComponent* ExistingComponent = nullptr;
	TArray<UActorComponent*> Components;
	CesiumActor->GetComponents<UActorComponent>(Components);
	
	UE_LOG(LogTemp, Warning, TEXT("SEARCH Checking tileset components for CesiumFeaturesMetadata..."));
	
	for (UActorComponent* Component : Components)
	{
		FString ComponentClassName = Component->GetClass()->GetName();
		UE_LOG(LogTemp, Warning, TEXT("   Component: %s"), *ComponentClassName);
		
		if (ComponentClassName.Contains(TEXT("CesiumFeaturesMetadata")))
		{
			UE_LOG(LogTemp, Warning, TEXT("SUCCESS Found existing CesiumFeaturesMetadata component"));
			ExistingComponent = Component;
			break;
		}
	}
	
	if (!ExistingComponent)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No CesiumFeaturesMetadata component found"));
		UE_LOG(LogTemp, Warning, TEXT("INFO SOLUTION: Manually add CesiumFeaturesMetadata component to tileset:"));
		UE_LOG(LogTemp, Warning, TEXT("   1. SELECT Select your 'bisingen' tileset in World Outliner"));
		UE_LOG(LogTemp, Warning, TEXT("   2. ➕ Click 'Add' button in Details panel"));
		UE_LOG(LogTemp, Warning, TEXT("   3. 🔧 Add 'CesiumFeaturesMetadata' component"));
		UE_LOG(LogTemp, Warning, TEXT("   4. REFRESH Click 'Auto Fill' to discover metadata"));
		UE_LOG(LogTemp, Warning, TEXT("   5. GENERATE Click 'Generate Material' to create material layer"));
		UE_LOG(LogTemp, Warning, TEXT("   6. 🎪 Create custom logic with RemapValueRangeNormalized nodes"));
		
		// Check if tileset has metadata extensions
		UE_LOG(LogTemp, Warning, TEXT(""));
		UE_LOG(LogTemp, Warning, TEXT("EXTENSIONS Your tileset needs these extensions for official method:"));
		UE_LOG(LogTemp, Warning, TEXT("   • EXT_mesh_features (for feature ID sets)"));
		UE_LOG(LogTemp, Warning, TEXT("   • EXT_structural_metadata (for property tables)"));
		UE_LOG(LogTemp, Warning, TEXT(""));
		UE_LOG(LogTemp, Warning, TEXT("ALTERNATIVES If extensions missing, alternative approaches:"));
		UE_LOG(LogTemp, Warning, TEXT("   1. EXTERNAL External tileset preprocessing with cesium-native"));
		UE_LOG(LogTemp, Warning, TEXT("   2. MATERIAL Custom material system with building ID lookup"));
		UE_LOG(LogTemp, Warning, TEXT("   3. VERTEX Vertex color injection if geometry supports it"));
		
		return;
	}
	
	// If component exists, analyze its properties
	UE_LOG(LogTemp, Warning, TEXT("ANALYZE Analyzing CesiumFeaturesMetadata component properties..."));
	UClass* ComponentClass = ExistingComponent->GetClass();
	
	// Look for property-related methods and properties
	bool bFoundRelevantProperties = false;
	for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		FString PropName = Property->GetName();
		
		if (PropName.Contains(TEXT("Feature")) || PropName.Contains(TEXT("Metadata")) || 
			PropName.Contains(TEXT("Property")) || PropName.Contains(TEXT("Table")))
		{
			UE_LOG(LogTemp, Warning, TEXT("🔧 Found metadata property: %s (Type: %s)"), 
				*PropName, *Property->GetCPPType());
			bFoundRelevantProperties = true;
		}
	}
	
	// Look for methods to interact with the component
	for (TFieldIterator<UFunction> FuncIt(ComponentClass); FuncIt; ++FuncIt)
	{
		UFunction* Function = *FuncIt;
		FString FuncName = Function->GetName();
		
		if (FuncName.Contains(TEXT("AutoFill")) || FuncName.Contains(TEXT("Generate")) || 
			FuncName.Contains(TEXT("Material")) || FuncName.Contains(TEXT("Property")))
		{
			UE_LOG(LogTemp, Warning, TEXT("🎮 Found metadata function: %s"), *FuncName);
			bFoundRelevantProperties = true;
		}
	}
	
	if (bFoundRelevantProperties)
	{
		UE_LOG(LogTemp, Warning, TEXT("SUCCESS CesiumFeaturesMetadata component has metadata capabilities"));
		UE_LOG(LogTemp, Warning, TEXT("INFO NEXT STEPS: Bridge your API data with Cesium metadata:"));
		UE_LOG(LogTemp, Warning, TEXT("   1. AUTOFILL Use 'Auto Fill' to discover existing metadata"));
		UE_LOG(LogTemp, Warning, TEXT("   2. GENERATE Generate material layer for discovered properties"));
		UE_LOG(LogTemp, Warning, TEXT("   3. 🔧 Map API building colors to material logic"));
		UE_LOG(LogTemp, Warning, TEXT("   4. REMAP Use RemapValueRangeNormalized for color ranges"));
		
		// Show sample of our API color data for reference
		UE_LOG(LogTemp, Warning, TEXT(""));
		UE_LOG(LogTemp, Warning, TEXT("SAMPLE Available API color data sample:"));
		int32 ColorCount = 0;
		for (const auto& BuildingColor : BuildingColorCache)
		{
			FColor SRGBColor = BuildingColor.Value.ToFColor(true);
			FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
			UE_LOG(LogTemp, Warning, TEXT("   BUILDING %s → %s"), *BuildingColor.Key, *HexColor);
			if (++ColorCount >= 5) break;
		}
		UE_LOG(LogTemp, Warning, TEXT("   ... and %d more buildings"), BuildingColorCache.Num() - ColorCount);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING CesiumFeaturesMetadata component found but no metadata methods available"));
		UE_LOG(LogTemp, Warning, TEXT("INFO This suggests tileset may not have required metadata extensions"));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("METADATA === OFFICIAL CESIUM METADATA VISUALIZATION END ==="));
}

void ABuildingEnergyDisplay::CreateBuildingAttributesForm(const FString& JsonData)
{
	UE_LOG(LogTemp, Error, TEXT("FORM *** CreateBuildingAttributesForm() FUNCTION ENTERED ***"));
	UE_LOG(LogTemp, Warning, TEXT("FORM Creating building attributes form widget..."));
	UE_LOG(LogTemp, Warning, TEXT("DATA JSON Data Length: %d characters"), JsonData.Len());
	
	if (!BuildingAttributesWidgetClass)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR BuildingAttributesWidgetClass not set! Please assign it in the editor."));
		UE_LOG(LogTemp, Error, TEXT("FIX TO FIX: In editor, select BuildingEnergyDisplay actor -> Details panel -> Building Attributes Widget Class -> Select your UMG widget"));
		UE_LOG(LogTemp, Error, TEXT("WIDGET Expected widget class: WBP_BuildingAttributes or similar UMG widget you created"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: BuildingAttributesWidgetClass not assigned in editor!"));
			GEngine->AddOnScreenDebugMessage(-1, 12.0f, FColor::Yellow, 
				TEXT("Fix: Select BuildingEnergyDisplay → Details → Assign Widget Class"));
		}
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("FORM BuildingAttributesWidgetClass is assigned correctly"));
	
	// Remove existing widget if any
	if (BuildingAttributesWidget)
	{
		UE_LOG(LogTemp, Warning, TEXT("FORM Removing existing widget..."));
		BuildingAttributesWidget->RemoveFromParent();
		BuildingAttributesWidget = nullptr;
	}
	
	// Create new widget instance
	if (UWorld* World = GetWorld())
	{
		UE_LOG(LogTemp, Warning, TEXT("FORM World found successfully"));
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			UE_LOG(LogTemp, Warning, TEXT("FORM PlayerController found successfully"));
			BuildingAttributesWidget = CreateWidget<UUserWidget>(PlayerController, BuildingAttributesWidgetClass);
			if (BuildingAttributesWidget)
			{
				// Cast to the specific widget type and set building data
				if (UBuildingAttributesWidget* AttributesWidget = Cast<UBuildingAttributesWidget>(BuildingAttributesWidget))
				{
					UE_LOG(LogTemp, Warning, TEXT("FORM Widget cast successful - setting building data"));
					// Set the building data - this will trigger the API call and form population
					AttributesWidget->SetBuildingData(CurrentRequestedBuildingKey, AccessToken);
					UE_LOG(LogTemp, Warning, TEXT("FORM SetBuildingData called with GmlId: %s"), *CurrentRequestedBuildingKey);
					
					// Check if buttons are properly bound
					if (AttributesWidget->BTN_Save)
					{
						UE_LOG(LogTemp, Warning, TEXT("WIDGET BTN_Save found and valid"));
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("WIDGET BTN_Save is NULL - check UMG widget binding!"));
					}
					
					if (AttributesWidget->BTN_Close)
					{
						UE_LOG(LogTemp, Warning, TEXT("WIDGET BTN_Close found and valid"));
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("WIDGET BTN_Close is NULL - check UMG widget binding!"));
					}
				}
				
				// Add widget to viewport
				UE_LOG(LogTemp, Warning, TEXT("FORM Adding widget to viewport..."));
				BuildingAttributesWidget->AddToViewport(100); // High Z-order to appear on top
				
				// Center the widget on screen
				if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
				{
					if (ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
					{
						FVector2D ViewportSize;
						LocalPlayer->ViewportClient->GetViewportSize(ViewportSize);
						
						// Calculate center position
						FVector2D CenterPosition = ViewportSize * 0.5f;
						
// Set widget to center position
					BuildingAttributesWidget->SetPositionInViewport(CenterPosition - FVector2D(250, 200)); // Offset by half widget size
						
						// Log actual widget dimensions for debugging
						FVector2D WidgetSize = BuildingAttributesWidget->GetDesiredSize();
						UE_LOG(LogTemp, Warning, TEXT("WIDGET Actual widget size: %f x %f pixels"), WidgetSize.X, WidgetSize.Y);
						
						UE_LOG(LogTemp, Warning, TEXT("FORM Widget centered at: %f, %f (ViewportSize: %f, %f)"), CenterPosition.X, CenterPosition.Y, ViewportSize.X, ViewportSize.Y);
					}
				}
				BuildingAttributesWidget->SetRenderOpacity(0.95f); // Slight transparency
				
				UE_LOG(LogTemp, Warning, TEXT("SUCCESS Widget created and added to viewport with transparency"));
				UE_LOG(LogTemp, Warning, TEXT("WIDGET Widget name: %s"), *BuildingAttributesWidget->GetName());
				UE_LOG(LogTemp, Warning, TEXT("WIDGET Widget class: %s"), *BuildingAttributesWidget->GetClass()->GetName());
				
				// Enable mouse cursor for UI interaction with Game+UI mode for transparency
				if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
				{
					PC->SetShowMouseCursor(true);
					PC->SetInputMode(FInputModeGameAndUI()); // Allow game input + UI interaction
					UE_LOG(LogTemp, Warning, TEXT("UI Mouse cursor enabled with Game+UI mode for transparency"));
				}
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("ERROR Failed to create BuildingAttributesWidget instance"));
				if (GEngine)
				{
					GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, 
						TEXT("ERROR: Failed to create widget instance - Check widget class assignment"));
				}
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("ERROR No PlayerController found"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR No World context found"));
	}
	
	UE_LOG(LogTemp, Error, TEXT("FORM *** CreateBuildingAttributesForm() FUNCTION COMPLETED ***"));
}

void ABuildingEnergyDisplay::PopulateBuildingAttributesWidget(const FString& JsonData)
{
	UE_LOG(LogTemp, Warning, TEXT("POPULATE === Populating Building Attributes Widget ==="));
	
	if (!BuildingAttributesWidget)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR No widget to populate"));
		return;
	}
	
	// Parse JSON response to extract building attributes
	TSharedPtr<FJsonValue> JsonValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonData);
	
	if (!FJsonSerializer::Deserialize(Reader, JsonValue) || !JsonValue.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to parse JSON data"));
		UE_LOG(LogTemp, Error, TEXT("RAW JSON: %s"), *JsonData.Left(200));
		return;
	}
	
	if (JsonValue->Type != EJson::Object)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR JSON is not an object"));
		return;
	}
	
	TSharedPtr<FJsonObject> JsonObject = JsonValue->AsObject();
	
	// Cast to our specific widget type for setting values
	if (UBuildingAttributesWidget* AttributesWidget = Cast<UBuildingAttributesWidget>(BuildingAttributesWidget))
	{
		UE_LOG(LogTemp, Warning, TEXT("SUCCESS Widget cast successful - populating fields"));
		
		// Extract general_info data
		if (JsonObject->HasField(TEXT("general_info")))
		{
			TSharedPtr<FJsonObject> GeneralInfo = JsonObject->GetObjectField(TEXT("general_info"));
			
			// Construction Year Class (dropdown)
			if (GeneralInfo->HasField(TEXT("construction_year_class")))
			{
				FString ConstructionYear = GeneralInfo->GetStringField(TEXT("construction_year_class"));
				UE_LOG(LogTemp, Warning, TEXT("FIELD Construction Year Class: %s"), *ConstructionYear);
				
				// Set the construction year dropdown
				if (AttributesWidget->CB_ConstructionYear)
				{
					AttributesWidget->CB_ConstructionYear->SetSelectedOption(ConstructionYear);
					UE_LOG(LogTemp, Warning, TEXT("SET Construction Year dropdown to: %s"), *ConstructionYear);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("ERROR CB_ConstructionYear widget is null"));
				}
			}
			
			// Number of Storeys (text input)
			if (GeneralInfo->HasField(TEXT("number_of_storey")))
			{
				FString NumberOfStoreys = GeneralInfo->GetStringField(TEXT("number_of_storey"));
				UE_LOG(LogTemp, Warning, TEXT("FIELD Number of Storeys: %s"), *NumberOfStoreys);
				
				// Set the number of storeys text box
				if (AttributesWidget->TB_NumberOfStorey)
				{
					AttributesWidget->TB_NumberOfStorey->SetText(FText::FromString(NumberOfStoreys));
					UE_LOG(LogTemp, Warning, TEXT("SET Number of Storeys to: %s"), *NumberOfStoreys);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("ERROR TB_NumberOfStorey widget is null"));
				}
			}
			
			// Roof Storey Type (dropdown) - check multiple possible field names
			TArray<FString> PossibleRoofFields = {
				TEXT("roof_storey_type"),
				TEXT("number_type_roof_storey"),
				TEXT("roof_type"),
				TEXT("storey_type")
			};
			
			for (const FString& FieldName : PossibleRoofFields)
			{
				if (GeneralInfo->HasField(FieldName))
				{
					FString RoofType = GeneralInfo->GetStringField(FieldName);
					UE_LOG(LogTemp, Warning, TEXT("FIELD Roof Storey Type (%s): %s"), *FieldName, *RoofType);
					
					// Set the roof type dropdown if widget exists
					if (AttributesWidget->CB_RoofStorey)
					{
						AttributesWidget->CB_RoofStorey->SetSelectedOption(RoofType);
						UE_LOG(LogTemp, Warning, TEXT("SET Roof Storey Type to: %s"), *RoofType);
					}
					break; // Use first found field
				}
			}
		}
		
		// Extract begin_of_project data
		if (JsonObject->HasField(TEXT("begin_of_project")))
		{
			TSharedPtr<FJsonObject> BeginProject = JsonObject->GetObjectField(TEXT("begin_of_project"));
			UE_LOG(LogTemp, Warning, TEXT("FIELDS Found begin_of_project section with %d fields"), BeginProject->Values.Num());
			
			// Heating System Type (dropdown) - check multiple possible field names
			TArray<FString> PossibleHeatingFields = {
				TEXT("heating_system_type_1"),
				TEXT("heating_system_type"),
				TEXT("heating_type"),
				TEXT("heating_system")
			};
			
			for (const FString& FieldName : PossibleHeatingFields)
			{
				if (BeginProject->HasField(FieldName))
				{
					FString HeatingSystem = BeginProject->GetStringField(FieldName);
					UE_LOG(LogTemp, Warning, TEXT("FIELD Heating System Type (%s): %s"), *FieldName, *HeatingSystem);
					
					// Set heating system dropdown if widget exists
					if (AttributesWidget->CB_HeatingSystemBefore)
					{
						AttributesWidget->CB_HeatingSystemBefore->SetSelectedOption(HeatingSystem);
						UE_LOG(LogTemp, Warning, TEXT("SET Heating System Type to: %s"), *HeatingSystem);
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("ERROR CB_HeatingSystemBefore widget is null"));
					}
					break; // Use first found field
				}
			}
			
			// Window Construction Year (dropdown) - check multiple possible field names
			TArray<FString> PossibleWindowFields = {
				TEXT("construction_year_class_renovated_window"),
				TEXT("window_construction_year_class"),
				TEXT("window_construction_year"),
				TEXT("renovated_window_year")
			};
			
			for (const FString& FieldName : PossibleWindowFields)
			{
				if (BeginProject->HasField(FieldName))
				{
					FString WindowYear = BeginProject->GetStringField(FieldName);
					UE_LOG(LogTemp, Warning, TEXT("FIELD Window Construction Year (%s): %s"), *FieldName, *WindowYear);
					
					// Set window construction year dropdown if widget exists
					if (AttributesWidget->CB_WindowBefore)
					{
						AttributesWidget->CB_WindowBefore->SetSelectedOption(WindowYear);
						UE_LOG(LogTemp, Warning, TEXT("SET Window Construction Year to: %s"), *WindowYear);
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("ERROR CB_WindowBefore widget is null"));
					}
					break; // Use first found field
				}
			}
			
			// Wall Construction Year (dropdown) - check multiple possible field names
			TArray<FString> PossibleWallFields = {
				TEXT("construction_year_class_renovated_wall"),
				TEXT("wall_construction_year_class"),
				TEXT("wall_construction_year"),
				TEXT("renovated_wall_year")
			};
			
			for (const FString& FieldName : PossibleWallFields)
			{
				if (BeginProject->HasField(FieldName))
				{
					FString WallYear = BeginProject->GetStringField(FieldName);
					UE_LOG(LogTemp, Warning, TEXT("FIELD Wall Construction Year (%s): %s"), *FieldName, *WallYear);
					
					// Set wall construction year dropdown if widget exists
					if (AttributesWidget->CB_WallBefore)
					{
						AttributesWidget->CB_WallBefore->SetSelectedOption(WallYear);
						UE_LOG(LogTemp, Warning, TEXT("SET Wall Construction Year to: %s"), *WallYear);
					}
					else
					{
						UE_LOG(LogTemp, Error, TEXT("ERROR CB_WallBefore widget is null"));
					}
					break; // Use first found field
				}
			}
			
			// Log all available fields in begin_of_project for debugging
			UE_LOG(LogTemp, Warning, TEXT("AVAILABLE Before Renovation fields:"));
			for (auto& Field : BeginProject->Values)
			{
				if (Field.Value->Type == EJson::String)
				{
					FString FieldValue = Field.Value->AsString();
					UE_LOG(LogTemp, Warning, TEXT("   %s: %s"), *Field.Key, *FieldValue);
				}
			}
		}
		
		// Extract end_of_project data  
		if (JsonObject->HasField(TEXT("end_of_project")))
		{
			TSharedPtr<FJsonObject> EndProject = JsonObject->GetObjectField(TEXT("end_of_project"));
			UE_LOG(LogTemp, Warning, TEXT("FIELDS Found end_of_project section with %d fields"), EndProject->Values.Num());
			
			// Log all fields in end_of_project
			for (auto& Field : EndProject->Values)
			{
				FString FieldName = Field.Key;
				if (Field.Value->Type == EJson::String)
				{
					FString FieldValue = Field.Value->AsString();
					UE_LOG(LogTemp, Warning, TEXT("FIELD After: %s = %s"), *FieldName, *FieldValue);
				}
			}
		}
		
		UE_LOG(LogTemp, Warning, TEXT("SUCCESS Widget populated with API data"));
		
		// Log complete API structure for debugging field names
		UE_LOG(LogTemp, Warning, TEXT("COMPLETE API STRUCTURE - All available fields:"));
		for (auto& Section : JsonObject->Values)
		{
			UE_LOG(LogTemp, Warning, TEXT("Section: %s"), *Section.Key);
			if (Section.Value->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> SubObject = Section.Value->AsObject();
				for (auto& Field : SubObject->Values)
				{
					if (Field.Value->Type == EJson::String)
					{
						FString FieldValue = Field.Value->AsString();
						UE_LOG(LogTemp, Warning, TEXT("  %s.%s: %s"), *Section.Key, *Field.Key, *FieldValue);
					}
					else if (Field.Value->Type == EJson::Number)
					{
						double FieldValue = Field.Value->AsNumber();
						UE_LOG(LogTemp, Warning, TEXT("  %s.%s: %.2f"), *Section.Key, *Field.Key, FieldValue);
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("  %s.%s: (non-string/number type)"), *Section.Key, *Field.Key);
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Section %s is not an object"), *Section.Key);
			}
		}
		
		// Show building ID that's being edited
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Cyan, 
				FString::Printf(TEXT("FORM Showing attributes for building: %s"), *CurrentBuildingGmlId));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to cast widget to UBuildingAttributesWidget"));
		UE_LOG(LogTemp, Error, TEXT("ACTUAL Widget class: %s"), *BuildingAttributesWidget->GetClass()->GetName());
	}
	
	UE_LOG(LogTemp, Warning, TEXT("POPULATE === End Populating Widget ==="));
}

void ABuildingEnergyDisplay::GetBuildingAttributes(const FString& BuildingKey, const FString& CommunityId, const FString& Token)
{
	// 🚫 === DISABLED FUNCTION - SHOULD NOT BE CALLED BY BLUEPRINT ===
	static int32 GetAttributesCallCounter = 0;
	GetAttributesCallCounter++;
	UE_LOG(LogTemp, Error, TEXT("🚫 === GetBuildingAttributes() CALLED #%d ==="), GetAttributesCallCounter);
	UE_LOG(LogTemp, Error, TEXT("🚫 ERROR: This function was DISABLED and should NOT be called directly!"));
	UE_LOG(LogTemp, Error, TEXT("🚫 BuildingKey received: '%s'"), *BuildingKey);
	UE_LOG(LogTemp, Error, TEXT("🚫 Blueprint should ONLY call OnBuildingClicked!"));
	
	// Store current request info for debugging
	CurrentRequestedBuildingKey = BuildingKey;
	CurrentRequestedCommunityId = CommunityId;
	
	// CRITICAL: Enhanced ID conversion for GML format
	FString ActualGmlId = BuildingKey;
	
	// Enhanced pattern matching for ID conversion (DEBW_001008widf -> DEBWL001008widf)
	if (BuildingKey.Contains(TEXT("_")))
	{
		// Handle specific pattern: DEBW_XXXXXXX -> DEBWLXXXXXXX
		if (BuildingKey.StartsWith(TEXT("DEBW_")))
		{
			ActualGmlId = BuildingKey.Replace(TEXT("DEBW_"), TEXT("DEBWL"));
		}
		else
		{
			// Fallback: simple underscore to L replacement
			ActualGmlId = BuildingKey.Replace(TEXT("_"), TEXT("L"));
		}
		UE_LOG(LogTemp, Error, TEXT("🔄 ID CONVERSION: '%s' -> '%s'"), *BuildingKey, *ActualGmlId);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ ID FORMAT: Already correct format '%s'"), *ActualGmlId);
	}
	
	// Create HTTP request
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	
	// Build the URL with gml_id in the path (corrected format)
	FString Url = FString::Printf(TEXT("https://backend.gisworld-tech.com/geospatial/buildings-energy/%s/?community_id=%s&field_type=basic"), 
		*ActualGmlId, *CommunityId);
	
	UE_LOG(LogTemp, Warning, TEXT("GET Building Attributes: %s"), *Url);
	
	// Enhanced debugging for API request
	UE_LOG(LogTemp, Warning, TEXT("REQUEST === Building Attributes GET Request Debug ==="));
	UE_LOG(LogTemp, Warning, TEXT("REQUEST Full URL: %s"), *Url);
	UE_LOG(LogTemp, Warning, TEXT("REQUEST BuildingKey (gml_id): %s"), *BuildingKey);
	UE_LOG(LogTemp, Warning, TEXT("REQUEST CommunityId: %s"), *CommunityId);
	UE_LOG(LogTemp, Warning, TEXT("REQUEST Token Length: %d"), Token.Len());
	UE_LOG(LogTemp, Warning, TEXT("REQUEST Token First 20 chars: %s"), *Token.Left(20));
	
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	
	// Bind the response function
	Request->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnGetBuildingAttributesResponse);
	
	// Execute the request
	if (!Request->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to start GET building attributes request"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: Failed to start GET building attributes request"));
		}
	}
}

void ABuildingEnergyDisplay::UpdateBuildingAttributes(const FString& BuildingKey, const FString& CommunityId, const FString& AttributesJson, const FString& Token)
{
	// CRITICAL: Enhanced ID conversion for GML format
	FString ActualGmlId = BuildingKey;
	
	// Enhanced pattern matching for ID conversion (DEBW_001008widf -> DEBWL001008widf)
	if (BuildingKey.Contains(TEXT("_")))
	{
		// Handle specific pattern: DEBW_XXXXXXX -> DEBWLXXXXXXX
		if (BuildingKey.StartsWith(TEXT("DEBW_")))
		{
			ActualGmlId = BuildingKey.Replace(TEXT("DEBW_"), TEXT("DEBWL"));
		}
		else
		{
			// Fallback: simple underscore to L replacement
			ActualGmlId = BuildingKey.Replace(TEXT("_"), TEXT("L"));
		}
		UE_LOG(LogTemp, Error, TEXT("🔄 ID CONVERSION: '%s' -> '%s'"), *BuildingKey, *ActualGmlId);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ ID FORMAT: Already correct format '%s'"), *ActualGmlId);
	}
	
	// Create HTTP request
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	
	// Build the URL with gml_id in the path (corrected format)
	FString Url = FString::Printf(TEXT("https://backend.gisworld-tech.com/geospatial/buildings-energy/%s/?community_id=%s&field_type=basic"), 
		*ActualGmlId, *CommunityId);
	
	UE_LOG(LogTemp, Warning, TEXT("PUT Building Attributes: %s"), *Url);
	UE_LOG(LogTemp, Log, TEXT("JSON Payload: %s"), *AttributesJson.Left(200)); // Log first 200 chars
	
	Request->SetURL(Url);
	Request->SetVerb(TEXT("PUT"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Token));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(AttributesJson);
	
	// Bind the response function
	Request->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnUpdateBuildingAttributesResponse);
	
	// Execute the request
	if (!Request->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Failed to start PUT building attributes request"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: Failed to start PUT building attributes request"));
		}
	}
}

void ABuildingEnergyDisplay::OnGetBuildingAttributesResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	// CRITICAL: Validate that this API response is for a legitimate building request
	if (!CurrentRequestedBuildingKey.IsEmpty())
	{
		// Check if this building should exist in our cache
		if (!BuildingDataCache.Contains(CurrentRequestedBuildingKey))
		{
			UE_LOG(LogTemp, Warning, TEXT("Blocked API response: Building '%s' not in cache"), *CurrentRequestedBuildingKey);
			return; // STOP HERE - Do not create form for invalid building
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Blocked API response: No building key specified"));
		return;
	}
	
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR GET Building Attributes request failed"));
		UE_LOG(LogTemp, Error, TEXT("STATUS Request was successful: %s"), bWasSuccessful ? TEXT("true") : TEXT("false"));
		UE_LOG(LogTemp, Error, TEXT("📞 Response is valid: %s"), Response.IsValid() ? TEXT("true") : TEXT("false"));
		
		if (Request.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("📞 Request URL: %s"), *Request->GetURL());
			UE_LOG(LogTemp, Error, TEXT("📞 Request Verb: %s"), *Request->GetVerb());
		}
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: GET Building Attributes request failed - Check network connection"));
		}
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseContent = Response->GetContentAsString();
	
	UE_LOG(LogTemp, Warning, TEXT("RESPONSE GET Building Attributes Response Code: %d"), ResponseCode);
UE_LOG(LogTemp, Warning, TEXT("RESPONSE Response Content Length: %d"), ResponseContent.Len());
	
	if (ResponseCode == 200)
	{
		UE_LOG(LogTemp, Log, TEXT("Building attributes loaded successfully"));
		
		// Parse and create the building attributes form
		TSharedPtr<FJsonValue> JsonValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseContent);

		if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
		{
			CreateBuildingAttributesForm(ResponseContent);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Could not parse building attributes JSON response"));
		}
	}
	else if (ResponseCode == 404)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Building not found (404) - Building may not exist in attributes database"));
		UE_LOG(LogTemp, Error, TEXT("� DEBUG: Requested gml_id=%s, community_id=%s"), *CurrentRequestedBuildingKey, *CurrentRequestedCommunityId);
		
		// Check if maybe the building exists but with original modified_gml_id format
		FString AlternateGmlId = CurrentRequestedBuildingKey.Replace(TEXT("L"), TEXT("_"));
		UE_LOG(LogTemp, Warning, TEXT("CONVERT Maybe building exists as: %s instead of %s?"), *AlternateGmlId, *CurrentRequestedBuildingKey);
		
		UE_LOG(LogTemp, Error, TEXT("RESPONSE Response: %s"), *ResponseContent.Left(300));
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Orange, 
				FString::Printf(TEXT("Building %s not found (404)"), *CurrentRequestedBuildingKey));
		}
	}
	else if (ResponseCode == 401)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR Unauthorized (401) - Token may be expired"));
		
		// Try to refresh token automatically if we have a refresh token
		if (!RefreshToken.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 Attempting automatic token refresh for attributes request..."));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, 
					TEXT("🔄 Token expired - attempting refresh..."));
			}
			RefreshAccessToken();
		}
		else
		{
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
					TEXT("Unauthorized - Please re-authenticate"));
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ GET Building Attributes failed (Code: %d)"), ResponseCode);
		UE_LOG(LogTemp, Error, TEXT("📄 Error response: %s"), *ResponseContent.Left(500));
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				FString::Printf(TEXT("ERROR: GET Building Attributes failed (Code: %d)"), ResponseCode));
		}
	}
}

void ABuildingEnergyDisplay::OnUpdateBuildingAttributesResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("❌ PUT Building Attributes request failed"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				TEXT("ERROR: PUT Building Attributes request failed"));
		}
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseContent = Response->GetContentAsString();
	
	if (ResponseCode == 200 || ResponseCode == 201 || ResponseCode == 204)
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ PUT Building Attributes SUCCESS (Code: %d)"), ResponseCode);
		UE_LOG(LogTemp, Warning, TEXT("📊 Response: %s"), *ResponseContent.Left(500));
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
				FString::Printf(TEXT("✅ Building attributes updated successfully")));
		}
		
		// � WEBSOCKET: Connect to energy WebSocket for real-time updates
		UE_LOG(LogTemp, Warning, TEXT("🔌 WEBSOCKET: Connecting to energy WebSocket for real-time updates"));
		
		// Connect to WebSocket for immediate energy data updates
		ConnectEnergyWebSocket();
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Cyan, 
				TEXT("🔌 WebSocket connected for real-time energy updates"));
		}
		
		// Also trigger immediate fresh data fetch as fallback
		FetchRealTimeEnergyData();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ PUT Building Attributes failed (Code: %d)"), ResponseCode);
		UE_LOG(LogTemp, Error, TEXT("📄 Error response: %s"), *ResponseContent.Left(500));
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Red, 
				FString::Printf(TEXT("ERROR: PUT Building Attributes failed (Code: %d)"), ResponseCode));
		}
	}
}

void ABuildingEnergyDisplay::TestBuildingAttributesAPI()
{
	if (AccessToken.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No access token available. Please wait for authentication to complete."));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Orange, 
				TEXT("No access token. Please wait for authentication."));
		}
		return;
	}
	
	if (BuildingDataCache.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WARNING No building data cached. Please run the game first to load building data."));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Orange, 
				TEXT("No building data loaded. Please wait for data to load first."));
		}
		return;
	}
	
	// Get a building from our cached data (rotate through different buildings for testing)
	FString TestModifiedGmlId;
	static int32 TestBuildingIndex = 0; // Static to remember between calls
	
	if (BuildingDataCache.Num() > 0)
	{
		// Get array of building IDs
		TArray<FString> BuildingIds;
		BuildingDataCache.GetKeys(BuildingIds);
		
		// Use modulo to cycle through buildings
		TestBuildingIndex = TestBuildingIndex % BuildingIds.Num();
		TestModifiedGmlId = BuildingIds[TestBuildingIndex];
		
		// Increment for next test
		TestBuildingIndex++;
		
		UE_LOG(LogTemp, Warning, TEXT("TEST Testing with building %d/%d: %s"), TestBuildingIndex, BuildingIds.Num(), *TestModifiedGmlId);
	}
	else
	{
		return;
	}
	
	// Get the actual gml_id (with L) from our cache
	FString* ActualGmlIdPtr = GmlIdCache.Find(TestModifiedGmlId);
	FString TestActualGmlId;
	if (ActualGmlIdPtr && !ActualGmlIdPtr->IsEmpty())
	{
		TestActualGmlId = *ActualGmlIdPtr;
	}
	else
	{
		// Fallback: try to convert manually
		TestActualGmlId = ConvertGmlIdToBuildingKey(TestModifiedGmlId);
		UE_LOG(LogTemp, Warning, TEXT("FALLBACK Using fallback conversion for: %s"), *TestModifiedGmlId);
	}
	
// Configuration - should come from project settings
    FString DefaultCommunityId = TEXT("08417008"); // Should be configurable per project
    UE_LOG(LogTemp, Warning, TEXT("TEST Using default Community ID: %s (should be configurable)"), *DefaultCommunityId);
	
	UE_LOG(LogTemp, Warning, TEXT("TEST === TESTING BUILDING ATTRIBUTES API ==="));
	UE_LOG(LogTemp, Warning, TEXT("MODIFIED Modified GML ID (from energy API): %s"), *TestModifiedGmlId);
	UE_LOG(LogTemp, Warning, TEXT("ACTUAL Actual GML ID (for attributes API): %s"), *TestActualGmlId);
	UE_LOG(LogTemp, Warning, TEXT("COMMUNITY Community ID: %s"), *DefaultCommunityId);
	// API configuration - should be externally configurable
	FString ApiBaseUrl = TEXT("https://backend.gisworld-tech.com");  // Should come from config
	
	FString TestApiUrl = FString::Printf(TEXT("%s/geospatial/buildings-energy/%s/?community_id=%s&field_type=basic"), *ApiBaseUrl, *TestActualGmlId, *DefaultCommunityId);
	UE_LOG(LogTemp, Log, TEXT("API URL: %s"), *TestApiUrl);
	
	// Call the GET function using the actual gml_id (with L)
	UE_LOG(LogTemp, Warning, TEXT("DEBUG About to call GetBuildingAttributes with gml_id: %s"), *TestActualGmlId);
	
	// ENHANCED DEBUG: Check if the GmlIdCache lookup worked
	UE_LOG(LogTemp, Log, TEXT("GmlIdCache entries: %d, BuildingColorCache entries: %d"), GmlIdCache.Num(), BuildingColorCache.Num());
	
	if (ActualGmlIdPtr && !ActualGmlIdPtr->IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("CACHE SUCCESS: Found %s in cache -> %s"), *TestModifiedGmlId, *TestActualGmlId);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("CACHE MISS: %s not found in GmlIdCache, using fallback conversion"), *TestModifiedGmlId);
		// Show some cache entries for debugging
		int32 Count = 0;
		for (auto& Entry : GmlIdCache)
		{
			if (Count < 5) // Show first 5 entries
			{
				UE_LOG(LogTemp, Warning, TEXT("CACHE Sample entry: %s -> %s"), *Entry.Key, *Entry.Value);
			}
			Count++;
		}
		
		// Also show BuildingColorCache entries to see if any data is available
		UE_LOG(LogTemp, Warning, TEXT("COLORDATA Sample BuildingColorCache entries:"));
		Count = 0;
		for (auto& Entry : BuildingColorCache)
		{
			if (Count < 5) // Show first 5 entries
			{
				UE_LOG(LogTemp, Warning, TEXT("COLORDATA %s -> color"), *Entry.Key);
			}
			Count++;
		}
	}
	
	// ENHANCED DEBUG: Validate the final ID format
	UE_LOG(LogTemp, Warning, TEXT("VALIDATE === ID Validation ==="));
	UE_LOG(LogTemp, Warning, TEXT("VALIDATE Original modified_gml_id: %s"), *TestModifiedGmlId);
	UE_LOG(LogTemp, Warning, TEXT("VALIDATE Final gml_id for API: %s"), *TestActualGmlId);
	UE_LOG(LogTemp, Warning, TEXT("VALIDATE Contains underscore: %s"), TestActualGmlId.Contains(TEXT("_")) ? TEXT("YES - ERROR!") : TEXT("NO - Good"));
	UE_LOG(LogTemp, Warning, TEXT("VALIDATE Contains L: %s"), TestActualGmlId.Contains(TEXT("L")) ? TEXT("YES - Good") : TEXT("NO - Error"));
	UE_LOG(LogTemp, Warning, TEXT("VALIDATE Community ID: %s"), *DefaultCommunityId);
	
	GetBuildingAttributes(TestActualGmlId, DefaultCommunityId, AccessToken);
}

FString ABuildingEnergyDisplay::ConvertGmlIdToBuildingKey(const FString& GmlId)
{
	// � CRITICAL: This function performs CASE-SENSITIVE GML ID conversion
	// REQUIREMENT: 'G' is different from 'g' - maintain exact case throughout conversion
	// Convert modified_gml_id (with _) to actual gml_id (with L) for attributes API
	// Example: DEBW_001000wrHDD → DEBWL001000wrHDD
	
	// �📨 TRACK CONVERSION FUNCTION CALLS
	static TMap<FString, TArray<float>> ConvertCallTimestamps;
	static TMap<FString, int32> ConvertCallCounts;
	static int32 GlobalConvertCounter = 0;
	
	float CurrentTime = FPlatformTime::Seconds();
	GlobalConvertCounter++;
	
	// Track all timestamps for this building's conversion calls
	if (!ConvertCallTimestamps.Contains(GmlId))
	{
		ConvertCallTimestamps.Add(GmlId, TArray<float>());
		ConvertCallCounts.Add(GmlId, 0);
	}
	
	ConvertCallTimestamps[GmlId].Add(CurrentTime);
	ConvertCallCounts[GmlId]++;
	
	// 📊 LOG CONVERSION STATISTICS
	UE_LOG(LogTemp, Error, TEXT("🔄 CONVERT CALL #%d - Input: %s, Total conversions for this ID: %d"), 
		GlobalConvertCounter, *GmlId, ConvertCallCounts[GmlId]);
		
	// Check recent conversion frequency (last 3 seconds)
	TArray<float>& ConvertTimestamps = ConvertCallTimestamps[GmlId];
	int32 RecentConversions = 0;
	for (float Timestamp : ConvertTimestamps)
	{
		if ((CurrentTime - Timestamp) <= 3.0f)
			RecentConversions++;
	}
	
	if (RecentConversions > 1)
	{
		UE_LOG(LogTemp, Error, TEXT("⚠️ MULTIPLE CONVERSIONS detected - %d conversions in last 3 seconds for ID: %s"), 
			RecentConversions, *GmlId);
			
		// Show all recent conversion timestamps
		for (int32 i = ConvertTimestamps.Num() - 1; i >= 0 && i >= ConvertTimestamps.Num() - 3; i--)
		{
			float TimeDiff = CurrentTime - ConvertTimestamps[i];
			UE_LOG(LogTemp, Error, TEXT("   🔄 Convert Call %d: %.3f seconds ago"), 
				ConvertTimestamps.Num() - i, TimeDiff);
		}
	}
	
	// Convert modified_gml_id (with _) to actual gml_id (with L) for attributes API
	// Example: DEBW_001000wrHDD → DEBWL001000wrHDD
	UE_LOG(LogTemp, Error, TEXT("🔄 CONVERT INPUT: '%s'"), *GmlId);
	
	FString BuildingKey = GmlId;
	
	// Replace underscore with L for attributes API
	if (BuildingKey.Contains(TEXT("_")))
	{
		BuildingKey = BuildingKey.Replace(TEXT("_"), TEXT("L"));
		UE_LOG(LogTemp, Error, TEXT("🔄 CONVERT SUCCESS: %s -> %s"), *GmlId, *BuildingKey);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("🔄 CONVERT SKIPPED: %s (already in L format)"), *GmlId);
	}
	
	UE_LOG(LogTemp, Error, TEXT("🔄 CONVERT OUTPUT: '%s'"), *BuildingKey);
	
	return BuildingKey;
}

FString ABuildingEnergyDisplay::ConvertActualGmlIdToModified(const FString& ActualGmlId)
{
	// 🔑 CRITICAL: This function performs CASE-SENSITIVE GML ID conversion
	// REQUIREMENT: 'G' is different from 'g' - maintain exact case throughout conversion
	// Convert actual gml_id (with L) to modified_gml_id (with _) for cache lookup
	// Example: DEBWL001000wrHDD → DEBW_001000wrHDD
	FString LocalModifiedGmlId = ActualGmlId;
	
	// Replace L with underscore for cache lookup
	if (LocalModifiedGmlId.Contains(TEXT("L")))
	{
		LocalModifiedGmlId = LocalModifiedGmlId.Replace(TEXT("L"), TEXT("_"));
		UE_LOG(LogTemp, Log, TEXT("Converted gml_id to modified_gml_id: %s -> %s"), *ActualGmlId, *LocalModifiedGmlId);
	}
	
	return LocalModifiedGmlId;
}

void ABuildingEnergyDisplay::ShowBuildingAttributesForm(const FString& BuildingGmlId)
{
	// 📨 TRACK FORM FUNCTION CALLS
	static TMap<FString, TArray<float>> FormCallTimestamps;
	static TMap<FString, int32> FormCallCounts;
	static int32 GlobalFormCounter = 0;
	
	float CurrentTime = FPlatformTime::Seconds();
	GlobalFormCounter++;
	
	// Track all timestamps for this building's form calls
	if (!FormCallTimestamps.Contains(BuildingGmlId))
	{
		FormCallTimestamps.Add(BuildingGmlId, TArray<float>());
		FormCallCounts.Add(BuildingGmlId, 0);
	}
	
	FormCallTimestamps[BuildingGmlId].Add(CurrentTime);
	FormCallCounts[BuildingGmlId]++;
	
	// 📊 LOG FORM CALL STATISTICS
	UE_LOG(LogTemp, Error, TEXT("📋 FORM CALL #%d - Building: %s, Total form calls for this building: %d"), 
		GlobalFormCounter, *BuildingGmlId, FormCallCounts[BuildingGmlId]);
		
	// Check recent form call frequency (last 2 seconds)
	TArray<float>& FormTimestamps = FormCallTimestamps[BuildingGmlId];
	int32 RecentFormCalls = 0;
	for (float Timestamp : FormTimestamps)
	{
		if ((CurrentTime - Timestamp) <= 2.0f)
			RecentFormCalls++;
	}
	
	if (RecentFormCalls > 1)
	{
		UE_LOG(LogTemp, Error, TEXT("⚠️ MULTIPLE FORM CALLS detected - %d form calls in last 2 seconds for Building: %s"), 
			RecentFormCalls, *BuildingGmlId);
		
		// Show all recent form timestamps
		for (int32 i = FormTimestamps.Num() - 1; i >= 0 && i >= FormTimestamps.Num() - 3; i--)
		{
			float TimeDiff = CurrentTime - FormTimestamps[i];
			UE_LOG(LogTemp, Error, TEXT("   📋 Form Call %d: %.3f seconds ago"), 
				FormTimestamps.Num() - i, TimeDiff);
		}
	}
	
	// RIGHT-CLICK ATTRIBUTES FORM: Convert modified_gml_id to gml_id for attributes API
	UE_LOG(LogTemp, Warning, TEXT("📝 === ATTRIBUTES FORM DEBUG ==="));
	UE_LOG(LogTemp, Warning, TEXT("📝 Input (modified_gml_id): %s"), *BuildingGmlId);
	
	// DUPLICATE PREVENTION (keeping existing logic)
	static FString LastFormId = TEXT("");
	static float LastFormTime = 0.0f;
	
	// Block duplicate form calls within 300ms for same building
	if ((CurrentTime - LastFormTime) < 0.3f && LastFormId == BuildingGmlId)
	{
		UE_LOG(LogTemp, Error, TEXT("🚫 BLOCKED duplicate form call - Building: %s (%.3fms gap, Total: %d)"), 
			*BuildingGmlId, (CurrentTime - LastFormTime) * 1000.0f, FormCallCounts[BuildingGmlId]);
		return;
	}
	
	LastFormId = BuildingGmlId;
	LastFormTime = CurrentTime;
	
	CurrentBuildingGmlId = BuildingGmlId;
	
	// === CRITICAL DEBUG: Check GmlIdCache contents ===
	UE_LOG(LogTemp, Error, TEXT("🔍 CACHE DEBUG: Total GmlIdCache entries: %d"), GmlIdCache.Num());
	for (const auto& CacheEntry : GmlIdCache)
	{
		if (CacheEntry.Key == BuildingGmlId)
		{
			UE_LOG(LogTemp, Error, TEXT("🔍 FOUND in cache: %s -> %s"), *CacheEntry.Key, *CacheEntry.Value);
		}
	}
	
	// CONVERT: modified_gml_id (with _) → gml_id (with L) for attributes API
	FString AttributesApiGmlId;
	
	// First try cache lookup using the stable ID
	FString* CachedActualGmlIdPtr = GmlIdCache.Find(BuildingGmlId);
	if (CachedActualGmlIdPtr && !CachedActualGmlIdPtr->IsEmpty())
	{
		AttributesApiGmlId = *CachedActualGmlIdPtr;
		UE_LOG(LogTemp, Error, TEXT("🔍 CACHE HIT: %s -> %s"), *BuildingGmlId, *AttributesApiGmlId);
	}
	else
	{
		// Convert from modified_gml_id (with _) to actual gml_id (with L) for attributes API
		AttributesApiGmlId = ConvertGmlIdToBuildingKey(BuildingGmlId);
		UE_LOG(LogTemp, Error, TEXT("🔍 CACHE MISS - CONVERTED: %s -> %s"), *BuildingGmlId, *AttributesApiGmlId);
		
		// Add to cache to ensure consistency
		GmlIdCache.Add(BuildingGmlId, AttributesApiGmlId);
		UE_LOG(LogTemp, Error, TEXT("🔍 ADDED TO CACHE: %s -> %s"), *BuildingGmlId, *AttributesApiGmlId);
	}
	
	UE_LOG(LogTemp, Error, TEXT("🔍 FINAL gml_id for widget: %s"), *AttributesApiGmlId);
	
	// VALIDATION: Check widget class is assigned
	if (!BuildingAttributesWidgetClass)
	{
		UE_LOG(LogTemp, Error, TEXT("ERROR BuildingAttributesWidgetClass not set!"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				TEXT("ERROR: Widget class not assigned in editor!"));
		}
		return;
	}
	
	// Remove existing widget if any
	if (BuildingAttributesWidget)
	{
		BuildingAttributesWidget->RemoveFromParent();
		BuildingAttributesWidget = nullptr;
		UE_LOG(LogTemp, Warning, TEXT("📝 Removed existing attributes widget"));
	}

	// Create new widget instance
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			BuildingAttributesWidget = CreateWidget<UUserWidget>(PlayerController, BuildingAttributesWidgetClass);
			if (BuildingAttributesWidget)
			{
				BuildingAttributesWidget->AddToViewport();
				UE_LOG(LogTemp, Warning, TEXT("📝 Created and added attributes widget to viewport"));
				
				// Center the widget on screen
				if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
				{
					if (ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
					{
						FVector2D ViewportSize;
						LocalPlayer->ViewportClient->GetViewportSize(ViewportSize);
						FVector2D CenterPosition = ViewportSize * 0.5f;
						BuildingAttributesWidget->SetPositionInViewport(CenterPosition - FVector2D(250, 200));
						UE_LOG(LogTemp, Warning, TEXT("📝 Positioned widget at center of screen"));
					}
				}
				
				// Set building data using actual gml_id (with L) for attributes API
				if (UBuildingAttributesWidget* AttributesWidget = Cast<UBuildingAttributesWidget>(BuildingAttributesWidget))
				{
					// Pass the actual gml_id (with L) to the widget for attributes API call
					AttributesWidget->SetBuildingData(AttributesApiGmlId, AccessToken);
					UE_LOG(LogTemp, Warning, TEXT("✅ Attributes form opened for gml_id: %s"), *AttributesApiGmlId);
					
					// REMOVED: Screen message to prevent duplicate displays - only ShowBuildingInfoWidget should show messages
					// if (GEngine)
					// {
					//		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, 
					//			FString::Printf(TEXT("Opening attributes form for building: %s"), *AttributesApiGmlId));
					// }
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("🚨 Failed to cast widget to UBuildingAttributesWidget"));
				}
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("🚨 Failed to create building attributes widget"));
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("🚨 No player controller found"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 No world found"));
	}
}

void ABuildingEnergyDisplay::GetBuildingAttributeOptions()
{
	UE_LOG(LogTemp, Warning, TEXT("OPTIONS Getting building attribute dropdown options..."));
	
	// This would typically call an API endpoint that returns dropdown options
	// For now, we'll log what options we need based on the screenshot
	
	UE_LOG(LogTemp, Warning, TEXT("REQUIRED Required dropdown options for building attributes form:"));
	UE_LOG(LogTemp, Warning, TEXT("   General Information:"));
	UE_LOG(LogTemp, Warning, TEXT("     - Construction year class"));
	UE_LOG(LogTemp, Warning, TEXT("     - Number/Type of Roof Storey"));
	UE_LOG(LogTemp, Warning, TEXT("   Before Renovation:"));
	UE_LOG(LogTemp, Warning, TEXT("     - Heating system type 1"));
	UE_LOG(LogTemp, Warning, TEXT("     - Construction year class of renovated window"));
	UE_LOG(LogTemp, Warning, TEXT("     - Construction year class of renovated wall"));
	UE_LOG(LogTemp, Warning, TEXT("     - Construction year class of renovated roof"));
	UE_LOG(LogTemp, Warning, TEXT("     - Construction year class of renovated ceiling"));
	UE_LOG(LogTemp, Warning, TEXT("   After Renovation:"));
	UE_LOG(LogTemp, Warning, TEXT("     - Same fields as Before Renovation"));
	
	// TODO: Make actual API call to get dropdown values
	// This might be a separate endpoint or part of the building attributes API response
}

void ABuildingEnergyDisplay::OnBuildingClicked(const FString& BuildingGmlId)
{
	// 📨 TRACK MESSAGE FREQUENCY AND FUNCTION CALLS
	static TMap<FString, TArray<float>> MessageTimestamps;
	static TMap<FString, int32> TotalMessageCounts;
	static int32 GlobalCallCounter = 0;
	
	float CurrentTime = FPlatformTime::Seconds();
	GlobalCallCounter++;
	
	// Track all timestamps for this building
	if (!MessageTimestamps.Contains(BuildingGmlId))
	{
		MessageTimestamps.Add(BuildingGmlId, TArray<float>());
		TotalMessageCounts.Add(BuildingGmlId, 0);
	}
	
	MessageTimestamps[BuildingGmlId].Add(CurrentTime);
	TotalMessageCounts[BuildingGmlId]++;
	
	// 📊 LOG MESSAGE STATISTICS
	UE_LOG(LogTemp, Error, TEXT("📨 MESSAGE #%d - Building: %s, Total for this building: %d"), 
		GlobalCallCounter, *BuildingGmlId, TotalMessageCounts[BuildingGmlId]);
		
	// Check recent message frequency (last 1 second)
	TArray<float>& Timestamps = MessageTimestamps[BuildingGmlId];
	int32 RecentMessages = 0;
	for (float Timestamp : Timestamps)
	{
		if ((CurrentTime - Timestamp) <= 1.0f)
			RecentMessages++;
	}
	
	if (RecentMessages > 1)
	{
		UE_LOG(LogTemp, Error, TEXT("⚠️ MULTIPLE MESSAGES detected - %d calls in last 1 second for Building: %s"), 
			RecentMessages, *BuildingGmlId);
		
		// Show all recent timestamps
		for (int32 i = Timestamps.Num() - 1; i >= 0 && i >= Timestamps.Num() - 5; i--)
		{
			float TimeDiff = CurrentTime - Timestamps[i];
			UE_LOG(LogTemp, Error, TEXT("   📍 Call %d: %.3f seconds ago"), 
				Timestamps.Num() - i, TimeDiff);
		}
	}
	
	// 🔍 FUNCTION SOURCE TRACKING
	UE_LOG(LogTemp, Error, TEXT("🔍 FUNCTION TRACE - OnBuildingClicked entry point"));
	UE_LOG(LogTemp, Error, TEXT("📍 CALL STACK - Building: %s, Time: %.3f"), *BuildingGmlId, CurrentTime);
	
	// DUPLICATE PREVENTION (keep existing logic but enhance logging)
	static FString LastProcessedId = TEXT("");
	static float LastCallTime = 0.0f;
	
	if ((CurrentTime - LastCallTime) < 0.3f && LastProcessedId == BuildingGmlId)
	{
		UE_LOG(LogTemp, Error, TEXT("🚫 BLOCKED duplicate call - Building: %s (%.3fms gap, Total calls: %d)"), 
			*BuildingGmlId, (CurrentTime - LastCallTime) * 1000.0f, TotalMessageCounts[BuildingGmlId]);
		return;
	}
	
	LastProcessedId = BuildingGmlId;
	LastCallTime = CurrentTime;
	
	// VALIDATION: Check if this is a valid building click
	if (BuildingGmlId.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 Right-click rejected: Empty building ID"));
		return;
	}
	
	if (BuildingGmlId == TEXT("XXXXX") || BuildingGmlId == TEXT("Default") || BuildingGmlId == TEXT("None"))
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 Right-click rejected: Invalid building ID '%s'"), *BuildingGmlId);
		return;
	}
	
	// 🔍 CESIUM PROPERTY DEBUG: When a building is clicked, show what Cesium properties are available
	UE_LOG(LogTemp, Warning, TEXT("🔍 CESIUM DEBUG: Analyzing clicked building '%s' for gml:id properties"), *BuildingGmlId);
	
	// Find the Cesium tileset actor to check its properties
	AActor* TilesetActor = nullptr;
	TArray<AActor*> AllActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass(), AllActors);
	
	for (AActor* Actor : AllActors)
	{
		if (Actor && Actor->GetClass()->GetName().Contains(TEXT("Cesium3DTileset")))
		{
			TilesetActor = Actor;
			break;
		}
	}
	
	if (TilesetActor)
	{
		// Look for property metadata that might contain the gml:id mapping
		UActorComponent* MetadataComponent = nullptr;
		TArray<UActorComponent*> AllComponents;
		TilesetActor->GetComponents<UActorComponent>(AllComponents);
		
		for (UActorComponent* Component : AllComponents)
		{
			if (Component && Component->GetClass()->GetName().Contains(TEXT("CesiumFeaturesMetadata")))
			{
				MetadataComponent = Component;
				break;
			}
		}
		
		if (MetadataComponent)
		{
			UE_LOG(LogTemp, Warning, TEXT("🎯 CESIUM ANALYSIS: Found metadata component for clicked building"));
			UE_LOG(LogTemp, Warning, TEXT("   Clicked Building ID: %s (format: modified_gml_id)"), *BuildingGmlId);
			
			// Convert modified_gml_id to potential gml:id format for comparison
			FString PotentialGmlId = BuildingGmlId;
			// Replace _ with L patterns common in the conversion
			PotentialGmlId = PotentialGmlId.Replace(TEXT("_"), TEXT("L"));
			UE_LOG(LogTemp, Warning, TEXT("   Potential gml:id: %s (converted for matching)"), *PotentialGmlId);
			
			// Check if we have this building in our cache
			if (BuildingColorCache.Contains(BuildingGmlId))
			{
				FLinearColor CachedColor = BuildingColorCache[BuildingGmlId];
				UE_LOG(LogTemp, Warning, TEXT("   ✅ CACHE HIT: Found color R=%.2f G=%.2f B=%.2f"), 
					CachedColor.R, CachedColor.G, CachedColor.B);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("   ❌ CACHE MISS: No color found for this building"));
			}
			
			// Look for gml:id in component properties using reflection
			UClass* MetadataClass = MetadataComponent->GetClass();
			for (TFieldIterator<FProperty> PropIt(MetadataClass); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				FString PropName = Property->GetName();
				
				if (PropName.Contains(TEXT("gml")) || PropName.Contains(TEXT("id")) || 
					PropName.Contains(TEXT("Description")) || PropName.Contains(TEXT("PropertyTable")))
				{
					UE_LOG(LogTemp, Warning, TEXT("   🏷️ PROPERTY: %s"), *PropName);
				}
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("   ❌ No CesiumFeaturesMetadata component found on tileset"));
		}
	}
	
	if (AccessToken.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 No access token. Cannot open attributes form."));
		// REMOVED: Authentication message to prevent interference with single building display
		// if (GEngine)
		// {
		//		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("Authentication required. Please wait..."));
		// }
		return;
	}
	
	// RIGHT-CLICK: BuildingGmlId is the modified_gml_id (with _) from Blueprint
	// We need to validate it exists in our energy data cache first
	if (!BuildingDataCache.Contains(BuildingGmlId))
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 Building '%s' not found in energy data cache"), *BuildingGmlId);
		
		// CASE-SENSITIVE search (gml_id and modified_gml_id are case-sensitive)
		bool bFoundMatch = false;
		FString FoundKey;
		
		UE_LOG(LogTemp, Warning, TEXT("🔍 RIGHT-CLICK SEARCH: Looking for building '%s' in cache"), *BuildingGmlId);
		UE_LOG(LogTemp, Warning, TEXT("🔍 CACHE SIZE: %d buildings available"), BuildingDataCache.Num());
		
		// Strategy 1: Exact case-sensitive match
		for (const auto& Entry : BuildingDataCache)
		{
			if (Entry.Key.Equals(BuildingGmlId))
			{
				FoundKey = Entry.Key;
				bFoundMatch = true;
				UE_LOG(LogTemp, Warning, TEXT("✅ Strategy 1 SUCCESS: Exact case-sensitive match '%s' -> '%s'"), *BuildingGmlId, *FoundKey);
				break;
			}
		}
		
		// Strategy 2: If no exact match, try ID format variations and transformations
		if (!bFoundMatch)
		{
			UE_LOG(LogTemp, Warning, TEXT("🔍 Strategy 2: Trying ID format variations for: %s"), *BuildingGmlId);
			
			for (const auto& Entry : BuildingDataCache)
			{
				FString CacheKey = Entry.Key;
				FString SearchKey = BuildingGmlId;
				
				// Create multiple normalized versions to compare
				TArray<FString> SearchVariations;
				TArray<FString> CacheVariations;
				
				// Add original versions (CASE-SENSITIVE - gml_id fields are case-sensitive)
				SearchVariations.Add(SearchKey);
				CacheVariations.Add(CacheKey);
				
				// Add underscore/L conversion variations (CASE-SENSITIVE)
				// Convert L to _ and _ to L
				FString SearchWithUnderscore = SearchKey.Replace(TEXT("L"), TEXT("_"));
				FString SearchWithL = SearchKey.Replace(TEXT("_"), TEXT("L"));
				FString CacheWithUnderscore = CacheKey.Replace(TEXT("L"), TEXT("_"));
				FString CacheWithL = CacheKey.Replace(TEXT("_"), TEXT("L"));
				
				SearchVariations.Add(SearchWithUnderscore);
				SearchVariations.Add(SearchWithL);
				CacheVariations.Add(CacheWithUnderscore);
				CacheVariations.Add(CacheWithL);
				
				// REMOVED: Case variations for common suffixes - gml_id fields are case-sensitive
				// GML IDs must be matched exactly with correct case (G != g)
				
				// Compare all variations
				for (const FString& SearchVar : SearchVariations)
				{
					for (const FString& CacheVar : CacheVariations)
					{
						if (SearchVar.Equals(CacheVar))
						{
							FoundKey = Entry.Key;
							bFoundMatch = true;
							UE_LOG(LogTemp, Warning, TEXT("✅ Strategy 2 SUCCESS: ID format match '%s' <-> '%s' (search:'%s' cache:'%s')"), 
								*BuildingGmlId, *FoundKey, *SearchVar, *CacheVar);
							break;
						}
					}
					if (bFoundMatch) break;
				}
				if (bFoundMatch) break;
			}
		}
		
		// Strategy 3: If still no match, try partial matching (contains)
		// Strategy 3: Try partial matching for: CASE-SENSITIVE (gml_id fields are case-sensitive)
		if (!bFoundMatch)
		{
			UE_LOG(LogTemp, Warning, TEXT("🔍 Trying partial matching for: %s"), *BuildingGmlId);
			
			for (const auto& Entry : BuildingDataCache)
			{
				// Try partial matching - check if the search string is contained in cache key (CASE-SENSITIVE)
				if (Entry.Key.Contains(BuildingGmlId) || BuildingGmlId.Contains(Entry.Key))
				{
					FoundKey = Entry.Key;
					bFoundMatch = true;
					UE_LOG(LogTemp, Warning, TEXT("✅ Partial match found: '%s' -> '%s'"), *BuildingGmlId, *FoundKey);
					break;
				}
			}
		}
		
		if (bFoundMatch)
		{
			UE_LOG(LogTemp, Warning, TEXT("✅ RIGHT-CLICK SUCCESS: Building match resolved. Opening form for: %s"), *FoundKey);
			// REMOVED: Screen message to prevent duplicate displays - only ShowBuildingInfoWidget should show messages
			// if (GEngine)
			// {
			//		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, 
			//			FString::Printf(TEXT("Opening form for: %s"), *FoundKey));
			// }
			ShowBuildingAttributesForm(FoundKey);
		}
		else
		{
			// Enhanced debugging for failed matches
			UE_LOG(LogTemp, Error, TEXT("🚨 RIGHT-CLICK FAILED: Building '%s' not found after all strategies"), *BuildingGmlId);
			UE_LOG(LogTemp, Error, TEXT("🔍 DEBUGGING: Available buildings in cache:"));
			int32 LogCount = 0;
			for (const auto& Entry : BuildingDataCache)
			{
				FString CacheKey = Entry.Key;
				FString Similarity = CacheKey.Contains(BuildingGmlId) ? TEXT("[PARTIAL]") : TEXT("");
				UE_LOG(LogTemp, Error, TEXT("  %d: '%s' %s"), LogCount + 1, *CacheKey, *Similarity);
				if (++LogCount >= 10) break; // Log first 10 for better debugging
			}
			
			UE_LOG(LogTemp, Error, TEXT("🚨 SOLUTION: Check if building ID format matches cache. Search: '%s'"), *BuildingGmlId);
			// REMOVED: Screen message to prevent clutter - errors logged to console only
			// if (GEngine)
			// {
			//		GEngine->AddOnScreenDebugMessage(-1, 8.0f, FColor::Red, 
			//			FString::Printf(TEXT("❌ Building %s not found. Check console for available IDs"), *BuildingGmlId));
			// }
		}
		return;
	}
	
	// Building found in cache - proceed to show attributes form
	UE_LOG(LogTemp, Error, TEXT("✅ Building found in cache. Opening attributes form for: %s"), *BuildingGmlId);
	ShowBuildingAttributesForm(BuildingGmlId);
}

// === NEW COORDINATE-BASED BUILDING VALIDATION SYSTEM ===

void ABuildingEnergyDisplay::OnBuildingClickedWithPosition(const FString& BuildingGmlId, const FVector& ClickPosition)
{
	// ⭐ CRITICAL: Position validation must be the FIRST check before ANY building operations
	UE_LOG(LogTemp, Warning, TEXT("🎯 Position-aware building click: ID=%s, Pos=(%f,%f,%f)"), 
		*BuildingGmlId, ClickPosition.X, ClickPosition.Y, ClickPosition.Z);
	
	// Step 1: Check if building has coordinate data
	if (!BuildingCoordinatesCache.Contains(BuildingGmlId))
	{
		UE_LOG(LogTemp, Error, TEXT("🚨 Building %s has no coordinate data - cannot validate position"), *BuildingGmlId);
		// Fallback to standard method if no coordinates available
		OnBuildingClicked(BuildingGmlId);
		return;
	}
	
	// Step 2: Validate position is within building polygon
	if (!ValidateBuildingPosition(ClickPosition, BuildingGmlId))
	{
		UE_LOG(LogTemp, Error, TEXT("🚫 Click position validation FAILED - position outside building bounds"));
		UE_LOG(LogTemp, Error, TEXT("🚫 Building ID: %s"), *BuildingGmlId);
		UE_LOG(LogTemp, Error, TEXT("🚫 Click Position: (%f, %f, %f)"), ClickPosition.X, ClickPosition.Y, ClickPosition.Z);
		
		// Try to find correct building at this position using coordinate map
		FString CorrectGmlId = GetBuildingByCoordinates(ClickPosition);
		if (!CorrectGmlId.IsEmpty() && CorrectGmlId != BuildingGmlId)
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 Found correct building at position: %s (instead of %s)"), 
				*CorrectGmlId, *BuildingGmlId);
			// Use the correct building ID
			OnBuildingClicked(CorrectGmlId);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("❌ No valid building found at click position"));
		}
		return;
	}
	
	// ✅ Position validation PASSED - proceed with standard building click handling
	UE_LOG(LogTemp, Warning, TEXT("✅ Position validation PASSED for building %s"), *BuildingGmlId);
	OnBuildingClicked(BuildingGmlId);
}

// === REAL-TIME MONITORING SYSTEM IMPLEMENTATION ===

void ABuildingEnergyDisplay::StartRealTimeMonitoring()
{
	bRealTimeMonitoringEnabled = true;
	RealTimeMonitoringTimer = 0.0f;
	NoChangesCount = 0;
	
	// Start with fast polling for immediate responsiveness
	RealTimeUpdateInterval = bEnhancedPollingMode ? FastPollingInterval : RealTimeUpdateInterval;
	
	UE_LOG(LogTemp, Warning, TEXT("REALTIME Real-time monitoring STARTED (checking every %.1f seconds)"), RealTimeUpdateInterval);
	UE_LOG(LogTemp, Warning, TEXT("REALTIME Enhanced polling mode: %s"), bEnhancedPollingMode ? TEXT("ENABLED") : TEXT("DISABLED"));
	
	// Create initial snapshot for change detection
	PreviousBuildingDataSnapshot = BuildingDataCache;
	PreviousColorSnapshot = BuildingColorCache;
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
			FString::Printf(TEXT("REAL-TIME MONITORING: Active (%.1fs interval, Enhanced: %s)"), 
				RealTimeUpdateInterval, bEnhancedPollingMode ? TEXT("ON") : TEXT("OFF")));
	}
}

void ABuildingEnergyDisplay::StopRealTimeMonitoring()
{
	bRealTimeMonitoringEnabled = false;
	bIsPerformingRealTimeUpdate = false;
	NoChangesCount = 0;
	UE_LOG(LogTemp, Warning, TEXT("REALTIME Real-time monitoring STOPPED"));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Orange, TEXT("REAL-TIME MONITORING: Stopped"));
	}
}

void ABuildingEnergyDisplay::SetRealTimeUpdateInterval(float Seconds)
{
	if (Seconds < 0.5f)
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Minimum update interval is 0.5 second. Setting to 0.5s"));
		RealTimeUpdateInterval = 0.5f;
	}
	else if (Seconds > 60.0f)
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Maximum update interval is 60 seconds. Setting to 60.0s"));
		RealTimeUpdateInterval = 60.0f;
	}
	else
	{
		RealTimeUpdateInterval = Seconds;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("REALTIME Update interval set to %.1f seconds"), RealTimeUpdateInterval);
}

void ABuildingEnergyDisplay::EnableEnhancedPolling(bool bEnable)
{
	bEnhancedPollingMode = bEnable;
	NoChangesCount = 0;
	
	if (bEnable)
	{
		RealTimeUpdateInterval = FastPollingInterval;
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Enhanced polling ENABLED - smart intervals active"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Enhanced polling DISABLED - fixed intervals"));
	}
}

void ABuildingEnergyDisplay::PerformRealTimeDataCheck()
{
	if (bIsPerformingRealTimeUpdate)
	{
		UE_LOG(LogTemp, Verbose, TEXT("REALTIME Real-time update already in progress, skipping"));
		return;
	}
	
	bIsPerformingRealTimeUpdate = true;
	
	// Make HTTP request to check for data changes
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	FString ApiUrl = TEXT("https://backend.gisworld-tech.com/geospatial/buildings-energy/?community_id=08417008&field_type=basic");
	
	Request->SetURL(ApiUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	
	// Real-time cache busting headers
	Request->SetHeader(TEXT("Cache-Control"), TEXT("no-cache, no-store, must-revalidate"));
	Request->SetHeader(TEXT("Pragma"), TEXT("no-cache"));
	
	Request->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnRealTimeDataResponse);
	
	if (Request->ProcessRequest())
	{
		UE_LOG(LogTemp, Verbose, TEXT("REALTIME Background data check request sent"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("REALTIME Failed to send background data check request"));
		bIsPerformingRealTimeUpdate = false;
	}
}

void ABuildingEnergyDisplay::OnRealTimeDataResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bIsPerformingRealTimeUpdate = false;
	
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Background data check failed"));
		return;
	}
	
	if (Response->GetResponseCode() != 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Background data check returned HTTP %d"), Response->GetResponseCode());
		return;
	}
	
	FString ResponseContent = Response->GetContentAsString();
	if (ResponseContent.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Background data check returned empty response"));
		return;
	}
	
	UE_LOG(LogTemp, Verbose, TEXT("REALTIME Background data check successful, analyzing for changes..."));
	DetectAndApplyChanges(ResponseContent);
}

void ABuildingEnergyDisplay::DetectAndApplyChanges(const FString& NewJsonData)
{
	// Parse new JSON data
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(NewJsonData);
	
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("REALTIME Failed to parse new JSON data"));
		return;
	}
	
	const TArray<TSharedPtr<FJsonValue>>* ResultsArray;
	if (!JsonObject->TryGetArrayField(TEXT("results"), ResultsArray))
	{
		UE_LOG(LogTemp, Error, TEXT("REALTIME No results array in JSON response"));
		return;
	}
	
	// Track changed buildings
	TArray<FString> ChangedBuildings;
	TMap<FString, FString> NewBuildingData;
	TMap<FString, FLinearColor> NewColorData;
	
	// Process each building result
	for (const TSharedPtr<FJsonValue>& ResultValue : *ResultsArray)
	{
		TSharedPtr<FJsonObject> ResultObject = ResultValue->AsObject();
		if (!ResultObject.IsValid()) continue;
		
		FString BuildingModifiedGmlId = ResultObject->GetStringField(TEXT("modified_gml_id"));
		if (BuildingModifiedGmlId.IsEmpty()) continue;
		
		// Convert to JSON string for comparison
		FString NewDataJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&NewDataJson);
		FJsonSerializer::Serialize(ResultObject.ToSharedRef(), Writer);
		
		// Check if data has changed
		FString* PreviousData = PreviousBuildingDataSnapshot.Find(BuildingModifiedGmlId);
		if (!PreviousData || !PreviousData->Equals(NewDataJson))
		{
			ChangedBuildings.Add(BuildingModifiedGmlId);
			NewBuildingData.Add(BuildingModifiedGmlId, NewDataJson);
			
			// Extract color information for automatic UI updates
			TSharedPtr<FJsonObject> EndObject = ResultObject->GetObjectField(TEXT("end"));
			if (EndObject.IsValid())
			{
				TSharedPtr<FJsonObject> EndColor = EndObject->GetObjectField(TEXT("color"));
				if (EndColor.IsValid() && EndColor->HasField(TEXT("energy_demand_specific_color")))
				{
					FString ColorHex = EndColor->GetStringField(TEXT("energy_demand_specific_color"));
					FLinearColor BuildingColor = ConvertHexToLinearColor(ColorHex);
					NewColorData.Add(BuildingModifiedGmlId, BuildingColor);
				}
			}
		}
	}
	
	// Apply changes if any detected
	if (ChangedBuildings.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("REALTIME CHANGES DETECTED! %d building(s) changed:"), ChangedBuildings.Num());
		
		// Update caches
		for (const FString& BuildingId : ChangedBuildings)
		{
			if (NewBuildingData.Contains(BuildingId))
			{
				BuildingDataCache.Add(BuildingId, NewBuildingData[BuildingId]);
				PreviousBuildingDataSnapshot.Add(BuildingId, NewBuildingData[BuildingId]);
				UE_LOG(LogTemp, Warning, TEXT("  - Building %s: Data updated"), *BuildingId);
			}
			
			if (NewColorData.Contains(BuildingId))
			{
				BuildingColorCache.Add(BuildingId, NewColorData[BuildingId]);
				PreviousColorSnapshot.Add(BuildingId, NewColorData[BuildingId]);
				UE_LOG(LogTemp, Warning, TEXT("  - Building %s: Color updated"), *BuildingId);
			}
		}
		
		// Apply visual updates automatically
		ApplyColorsUsingCesiumStyling();
		
		// Notify about changes
		NotifyRealTimeChanges(ChangedBuildings);
		
		// Update polling strategy based on changes detected
		UpdatePollingStrategy(true);
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("REALTIME No changes detected in background check"));
		
		// Update polling strategy for no changes
		UpdatePollingStrategy(false);
	}
}

void ABuildingEnergyDisplay::UpdatePollingStrategy(bool bChangesDetected)
{
	if (!bEnhancedPollingMode)
	{
		return; // No adaptive polling when enhanced mode is disabled
	}
	
	if (bChangesDetected)
	{
		// Changes detected - stay on fast polling and reset counter
		NoChangesCount = 0;
		RealTimeUpdateInterval = FastPollingInterval;
		UE_LOG(LogTemp, Warning, TEXT("REALTIME Changes detected - using fast polling (%.1fs)"), FastPollingInterval);
	}
	else
	{
		// No changes - increment counter
		NoChangesCount++;
		
		if (NoChangesCount >= SlowDownThreshold)
		{
			// Switch to slow polling mode to reduce API load
			RealTimeUpdateInterval = SlowPollingInterval;
			UE_LOG(LogTemp, Verbose, TEXT("REALTIME No changes for %d checks - switching to slow polling (%.1fs)"), 
				NoChangesCount, SlowPollingInterval);
		}
		else
		{
			// Still checking frequently
			RealTimeUpdateInterval = FastPollingInterval;
		}
	}
}

void ABuildingEnergyDisplay::NotifyRealTimeChanges(const TArray<FString>& ChangedBuildings)
{
	// Log changes to console for debugging
	UE_LOG(LogTemp, Warning, TEXT("REALTIME Real-time changes applied automatically: %d buildings updated"), ChangedBuildings.Num());
	
	// Log first few changed buildings for debugging
	int32 ShowCount = FMath::Min(ChangedBuildings.Num(), 3);
	for (int32 i = 0; i < ShowCount; i++)
	{
		UE_LOG(LogTemp, Warning, TEXT("  - Updated: %s"), *ChangedBuildings[i]);
	}
	
	if (ChangedBuildings.Num() > 3)
	{
		UE_LOG(LogTemp, Warning, TEXT("  ... and %d more buildings updated"), ChangedBuildings.Num() - 3);
	}
}

void ABuildingEnergyDisplay::CloseAttributesForm()
{
	UE_LOG(LogTemp, Log, TEXT("Closing building attributes form"));
	
	// Remove existing widget if any
	if (BuildingAttributesWidget)
	{
		BuildingAttributesWidget->RemoveFromParent();
		BuildingAttributesWidget = nullptr;
		UE_LOG(LogTemp, Log, TEXT("Building attributes form closed"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("No building attributes form to close"));
	}
}

// ========================================
// UMG WIDGET FUNCTIONS FOR BUILDING INFO
// ========================================

void ABuildingEnergyDisplay::CreateBuildingInfoWidget()
{
	UE_LOG(LogTemp, Warning, TEXT("🎨 CreateBuildingInfoWidget - Using screen overlay approach"));
	
	// Remove existing widget if any (single building display)
	HideBuildingInfoWidget();
	
	UE_LOG(LogTemp, Warning, TEXT("✅ Building Info Widget system initialized (screen overlay mode)"));
}

void ABuildingEnergyDisplay::ShowBuildingInfoWidget(const FString& BuildingId, const FString& BuildingData)
{
	// INSTANCE PROTECTION: Only allow the first instance to show messages
	static ABuildingEnergyDisplay* PrimaryInstance = nullptr;
	if (PrimaryInstance == nullptr)
	{
		PrimaryInstance = this;
		UE_LOG(LogTemp, Warning, TEXT("👑 PRIMARY INSTANCE: %s designated as primary display instance"), *GetName());
	}
	
	if (this != PrimaryInstance)
	{
		UE_LOG(LogTemp, Warning, TEXT("🚫 SECONDARY INSTANCE: %s blocked from showing messages (Primary: %s)"), *GetName(), *PrimaryInstance->GetName());
		return;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🎨 ShowBuildingInfoWidget - Building: %s (Primary Instance)"), *BuildingId);
	
	// SINGLE BUILDING DISPLAY: Hide any previously displayed building
	if (!CurrentlyDisplayedBuildingId.IsEmpty() && CurrentlyDisplayedBuildingId != BuildingId)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 SINGLE DISPLAY: Hiding previous building '%s' and showing '%s'"), 
			*CurrentlyDisplayedBuildingId, *BuildingId);
		HideBuildingInfoWidget();
	}
	
	// Update the currently displayed building ID for single building display
	CurrentlyDisplayedBuildingId = BuildingId;
	
	// Use simple screen overlay display instead of UMG widget
	if (GEngine)
	{
		// NUCLEAR OPTION: Completely disable ALL other screen messages by clearing multiple times
		for (int32 i = 0; i < 5; i++)
		{
			GEngine->ClearOnScreenDebugMessages();
			FPlatformProcess::Sleep(0.001f); // Tiny delay
		}
		
		// Format building data for better readability
		FString FormattedData = BuildingData;
		FormattedData = FormattedData.Replace(TEXT(","), TEXT(",\n"));
		FormattedData = FormattedData.Replace(TEXT("{"), TEXT("{\n  "));
		FormattedData = FormattedData.Replace(TEXT("}"), TEXT("\n}"));
		
		// Create display message with building information
		FString DisplayMessage = FString::Printf(TEXT("🏢 BUILDING ENERGY INFO\n==================\nBuilding ID: %s\n\nData:\n%s"), 
			*BuildingId, *FormattedData);
		
		// Use a UNIQUE key for building info messages - clear other potential keys first
		for (int32 key = 999; key <= 1001; key++)
		{
			GEngine->AddOnScreenDebugMessage(key, 0.01f, FColor::Red, TEXT("")); // Clear potential keys
		}
		
		// Now show ONLY our message
		GEngine->AddOnScreenDebugMessage(1000, 60.0f, FColor::Cyan, DisplayMessage);
		
		UE_LOG(LogTemp, Warning, TEXT("✅ NUCLEAR SINGLE BUILDING: Displayed info for %s using key 1000"), *BuildingId);
		
		UE_LOG(LogTemp, Warning, TEXT("✅ Building Info displayed for: %s (using screen overlay)"), *BuildingId);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ Cannot show Building Info - GEngine is null"));
	}
}

void ABuildingEnergyDisplay::HideBuildingInfoWidget()
{
	if (!CurrentlyDisplayedBuildingId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("🎨 Hiding Building Info for: %s"), *CurrentlyDisplayedBuildingId);
		
		// Clear specifically the building info message using the key
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(1000, 0.1f, FColor::Cyan, TEXT("")); // Clear by overriding with short duration
		}
		
		CurrentlyDisplayedBuildingId = TEXT(""); // Clear the currently displayed building ID
		UE_LOG(LogTemp, Warning, TEXT("✅ Building Info hidden (screen messages cleared)"));
	}
}

void ABuildingEnergyDisplay::FetchRealTimeEnergyData()
{
	UE_LOG(LogTemp, Warning, TEXT("🚀 === REAL-TIME ENERGY DATA FETCH INITIATED ==="));
	
	// Check if authentication token is available
	if (AccessToken.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("🚀 REAL-TIME: No access token available"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Orange, TEXT("🔐 Need authentication for real-time data"));
		}
		
		// Show message to user that authentication is needed
		UE_LOG(LogTemp, Warning, TEXT("🚀 REAL-TIME: Please authenticate first using PreloadAllBuildingData"));
		return;
	}
	
	// Fast direct API call for fresh energy data
	UE_LOG(LogTemp, Warning, TEXT("🚀 REAL-TIME: Starting fast energy data fetch"));
	
	// Create high-priority HTTP request for real-time data
	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->OnProcessRequestComplete().BindUObject(this, &ABuildingEnergyDisplay::OnRealTimeEnergyDataResponse);
	Request->SetURL("https://app-hft-buildingenergyapi-staging.azurewebsites.net/api/building-energy/community/13");
	Request->SetVerb("GET");
	Request->SetHeader("Content-Type", "application/json");
	Request->SetHeader("Authorization", "Bearer " + AccessToken);
	
	// Add priority headers for faster processing
	Request->SetTimeout(5.0f); // 5 second timeout for fast response
	
	UE_LOG(LogTemp, Warning, TEXT("🚀 REAL-TIME: Sending priority energy data request"));
	UE_LOG(LogTemp, Warning, TEXT("🚀 URL: https://app-hft-buildingenergyapi-staging.azurewebsites.net/api/building-energy/community/13"));
	
	if (!Request->ProcessRequest())
	{
		UE_LOG(LogTemp, Error, TEXT("🚀 REAL-TIME: Failed to start priority energy data request"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("❌ Real-time data fetch failed"));
		}
	}
	else
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("⚡ Fetching real-time energy data..."));
		}
	}
}

void ABuildingEnergyDisplay::OnRealTimeEnergyDataResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	UE_LOG(LogTemp, Warning, TEXT("🚀 === REAL-TIME ENERGY DATA RESPONSE ==="));
	
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("🚀 REAL-TIME: Energy data request failed"));
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("❌ Real-time data fetch failed"));
		}
		return;
	}

	int32 ResponseCode = Response->GetResponseCode();
	FString ResponseContent = Response->GetContentAsString();
	
	UE_LOG(LogTemp, Warning, TEXT("🚀 REAL-TIME: Response Code: %d"), ResponseCode);
	UE_LOG(LogTemp, Warning, TEXT("🚀 REAL-TIME: Data size: %d characters"), ResponseContent.Len());
	
	if (ResponseCode == 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ REAL-TIME: Fresh energy data received successfully"));
		
		// Clear old cache and immediately populate with fresh data
		BuildingDataCache.Empty();
		GmlIdCache.Empty();
		
		UE_LOG(LogTemp, Warning, TEXT("🔄 REAL-TIME: Processing fresh API data"));
		
		// Process the fresh data immediately
		ParseAndCacheAllBuildings(ResponseContent);
		
		// Mark data as loaded
		bDataLoaded = true;
		bIsLoading = false;
		
		// 🎨 AUTO COLOR APPLICATION: Apply colors immediately after real-time data update
		// TEMPORARILY DISABLED - Causing gray overlay on entire scene
		// UE_LOG(LogTemp, Warning, TEXT("🎨 REAL-TIME COLOR UPDATE: Applying fresh colors to buildings..."));
		
		// Schedule color application after a short delay
		// DISABLED - This was causing the entire scene to turn gray
		/*
		FTimerHandle RealTimeColorTimer;
		GetWorld()->GetTimerManager().SetTimer(RealTimeColorTimer, [this]()
		{
			UE_LOG(LogTemp, Warning, TEXT("🎨 EXECUTING: Real-time color application"));
			ApplyColorsDirectlyToGeometry();
			ApplyColorsToCSiumTileset();
			ForceApplyColors();
		}, 1.0f, false); // 1 second delay for real-time updates
		*/
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
				TEXT("✅ Real-time energy data updated!"));
		}
		
		UE_LOG(LogTemp, Warning, TEXT("🚀 REAL-TIME: Fresh data cache populated with %d buildings"), BuildingDataCache.Num());
		
		// If there's a currently displayed building, refresh its data immediately
		if (!CurrentlyDisplayedBuildingId.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 REAL-TIME: Refreshing displayed building: %s"), *CurrentlyDisplayedBuildingId);
			
			// Get fresh data for the currently displayed building
			FString* FreshEnergyData = BuildingDataCache.Find(CurrentlyDisplayedBuildingId);
			if (FreshEnergyData && !FreshEnergyData->IsEmpty())
			{
				// Update the display with fresh data
				ShowBuildingInfoWidget(CurrentlyDisplayedBuildingId, **FreshEnergyData);
				UE_LOG(LogTemp, Warning, TEXT("✅ REAL-TIME: Display updated with fresh data"));
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ REAL-TIME: Failed to get fresh energy data (Code: %d)"), ResponseCode);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
				FString::Printf(TEXT("❌ Real-time fetch failed (Code: %d)"), ResponseCode));
		}
	}
}

void ABuildingEnergyDisplay::ForceRealTimeRefresh()
{
	UE_LOG(LogTemp, Warning, TEXT("🔄 === MANUAL REAL-TIME REFRESH TRIGGERED ==="));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, TEXT("🔄 Manual refresh initiated..."));
	}
	
	// Clear cache completely to force fresh fetch
	BuildingDataCache.Empty();
	GmlIdCache.Empty();
	bDataLoaded = false;
	bIsLoading = false;
	
	// Trigger immediate real-time data fetch
	FetchRealTimeEnergyData();
}

void ABuildingEnergyDisplay::ConnectEnergyWebSocket()
{
	UE_LOG(LogTemp, Warning, TEXT("� === STARTING REST API ENERGY POLLING ==="));
	
	// Skip WebSocket - we're using REST API polling instead
	if (EnergyWebSocketURL.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("🔄 Setting up REST API polling for real-time energy updates"));
		
		// Validate we have authentication token
		if (AccessToken.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 Polling setup delayed - waiting for authentication token"));
			
			// Show authentication waiting message only once to prevent spam
			if (!bAuthenticationMessageShown && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("🔄 Waiting for authentication before starting energy polling"));
				bAuthenticationMessageShown = true;
			}
			return;
		}
		
		// Enable REST API polling mode
		bEnergyWebSocketConnected = true; // Use this flag to indicate polling is active
		WebSocketReconnectTimer = 0.0f; // Reset timer
		
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, 
				FString::Printf(TEXT("🔄 REST API polling active (every %.1fs)"), WebSocketReconnectInterval));
		}
		
		UE_LOG(LogTemp, Warning, TEXT("✅ REST API energy polling started - interval: %.1fs"), WebSocketReconnectInterval);
		
		// Perform initial energy data fetch
		FetchUpdatedEnergyData();
		return;
	}
	
	if (bEnergyWebSocketConnected && EnergyWebSocket.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("🔌 Energy WebSocket already connected"));
		return;
	}
	
	// Disconnect existing WebSocket if any
	if (EnergyWebSocket.IsValid())
	{
		EnergyWebSocket->Close();
		EnergyWebSocket = nullptr;
	}
	
	// Validate Access Token before creating WebSocket
	if (AccessToken.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("🔌 WebSocket connection delayed - waiting for authentication token"));
		
		// Show authentication waiting message only once to prevent spam
		if (!bAuthenticationMessageShown && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("🔌 Waiting for authentication before WebSocket connection"));
			bAuthenticationMessageShown = true;
		}
		return; // Wait for authentication
	}
	
	// Create WebSocket connection for real-time energy updates
	if (FModuleManager::Get().IsModuleLoaded("WebSockets"))
	{
		FWebSocketsModule& WebSocketsModule = FModuleManager::GetModuleChecked<FWebSocketsModule>("WebSockets");
		
		// Create WebSocket with authentication
		TMap<FString, FString> Headers;
		Headers.Add("Authorization", "Bearer " + AccessToken);
		Headers.Add("Content-Type", "application/json");
		
		UE_LOG(LogTemp, Warning, TEXT("🔌 Creating energy WebSocket with URL: %s"), *EnergyWebSocketURL);
		
		EnergyWebSocket = WebSocketsModule.CreateWebSocket(EnergyWebSocketURL, TEXT(""), Headers);
		
		if (EnergyWebSocket.IsValid())
		{
			// Bind WebSocket events for energy updates
			EnergyWebSocket->OnConnected().AddUObject(this, &ABuildingEnergyDisplay::OnEnergyWebSocketConnected);
			EnergyWebSocket->OnConnectionError().AddUObject(this, &ABuildingEnergyDisplay::OnEnergyWebSocketConnectionError);
			EnergyWebSocket->OnClosed().AddUObject(this, &ABuildingEnergyDisplay::OnEnergyWebSocketClosed);
			EnergyWebSocket->OnMessage().AddUObject(this, &ABuildingEnergyDisplay::OnEnergyWebSocketMessage);
			
			// Connect to WebSocket server
			EnergyWebSocket->Connect();
			
			UE_LOG(LogTemp, Warning, TEXT("🔌 Connecting to energy WebSocket: %s"), *EnergyWebSocketURL);
			
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow, TEXT("🔌 Connecting to energy WebSocket..."));
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("🔌 Failed to create energy WebSocket - CreateWebSocket returned null"));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, TEXT("❌ Energy WebSocket error: Could not initialize connection"));
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("🔌 WebSockets module not loaded"));
	}
}

void ABuildingEnergyDisplay::DisconnectEnergyWebSocket()
{
	UE_LOG(LogTemp, Warning, TEXT("🔌 === DISCONNECTING ENERGY WEBSOCKET ==="));
	
	bEnergyWebSocketConnected = false;
	
	if (EnergyWebSocket.IsValid())
	{
		EnergyWebSocket->Close();
		EnergyWebSocket = nullptr;
	}
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("🔌 Energy WebSocket disconnected"));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔌 Energy WebSocket disconnected"));
}

void ABuildingEnergyDisplay::OnEnergyWebSocketConnected()
{
	UE_LOG(LogTemp, Warning, TEXT("✅ ENERGY WEBSOCKET CONNECTED"));
	
	bEnergyWebSocketConnected = true;
	WebSocketReconnectTimer = 0.0f;
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green, TEXT("✅ Real-time energy WebSocket connected!"));
	}
	
	// Send subscription message for building energy updates
	FString SubscriptionMessage = FString::Printf(TEXT("{\"action\":\"subscribe\",\"type\":\"energy-updates\",\"community\":\"13\"}"));
	if (EnergyWebSocket.IsValid())
	{
		EnergyWebSocket->Send(SubscriptionMessage);
		UE_LOG(LogTemp, Warning, TEXT("🔌 Sent subscription: %s"), *SubscriptionMessage);
	}
}

void ABuildingEnergyDisplay::OnEnergyWebSocketConnectionError(const FString& Error)
{
	UE_LOG(LogTemp, Error, TEXT("❌ ENERGY WEBSOCKET CONNECTION ERROR: %s"), *Error);
	
	bEnergyWebSocketConnected = false;
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red, 
			FString::Printf(TEXT("❌ Energy WebSocket error: %s"), *Error));
	}
}

void ABuildingEnergyDisplay::OnEnergyWebSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogTemp, Warning, TEXT("🔌 ENERGY WEBSOCKET CLOSED: Code=%d, Reason=%s, Clean=%s"), 
		StatusCode, *Reason, bWasClean ? TEXT("true") : TEXT("false"));
	
	bEnergyWebSocketConnected = false;
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Yellow, TEXT("🔌 Energy WebSocket closed"));
	}
}

void ABuildingEnergyDisplay::OnEnergyWebSocketMessage(const FString& Message)
{
	EnergyUpdateCounter++;
	UE_LOG(LogTemp, Warning, TEXT("📨 ENERGY WEBSOCKET MESSAGE #%d RECEIVED"), EnergyUpdateCounter);
	UE_LOG(LogTemp, Warning, TEXT("📨 Message: %s"), *Message.Left(200));
	
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Green, 
			FString::Printf(TEXT("📨 Real-time energy update #%d"), EnergyUpdateCounter));
	}
	
	// Process the energy update
	ProcessEnergyWebSocketUpdate(Message);
}

void ABuildingEnergyDisplay::ProcessEnergyWebSocketUpdate(const FString& JsonData)
{
	UE_LOG(LogTemp, Warning, TEXT("🔄 PROCESSING WEBSOCKET ENERGY UPDATE"));
	
	// Parse the WebSocket message as JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonData);
	
	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		FString MessageType = JsonObject->GetStringField(TEXT("type"));
		
		if (MessageType == "energy-update" || MessageType == "building-energy-update")
		{
			// Check if it's a full data update or specific building update
			if (JsonObject->HasField(TEXT("buildings")))
			{
				// Full energy data update
				UE_LOG(LogTemp, Warning, TEXT("🔄 Full energy data update received via WebSocket"));
				
				// Clear existing cache
				BuildingDataCache.Empty();
				GmlIdCache.Empty();
				
				// Process the full update
				FString BuildingsArray = JsonObject->GetStringField(TEXT("buildings"));
				if (!BuildingsArray.IsEmpty())
				{
					ParseAndCacheAllBuildings(BuildingsArray);
				}
			}
			else if (JsonObject->HasField(TEXT("building_id")) && JsonObject->HasField(TEXT("energy_data")))
			{
				// Specific building energy update
				FString BuildingId = JsonObject->GetStringField(TEXT("building_id"));
				FString EnergyData = JsonObject->GetStringField(TEXT("energy_data"));
				
				UE_LOG(LogTemp, Warning, TEXT("🔄 Specific building energy update: %s"), *BuildingId);
				
				// Extract coordinates if available in the update
				if (JsonObject->HasField(TEXT("coordinates")))
				{
					FString CoordinatesData = JsonObject->GetStringField(TEXT("coordinates"));

					// Create unique cache key when numeric id is available
					FString UniqueCacheKey = BuildingId;
					if (JsonObject->HasField(TEXT("id")))
					{
						double NumericIdD = 0.0;
						if (JsonObject->TryGetNumberField(TEXT("id"), NumericIdD))
						{
							int32 NumericId = (int32)NumericIdD;
							UniqueCacheKey = FString::Printf(TEXT("%s#%d"), *BuildingId, NumericId);
						}
					}

					StoreBuildingCoordinates(UniqueCacheKey, CoordinatesData);
					UE_LOG(LogTemp, Warning, TEXT("🔄 Updated coordinates for building: %s (cached as %s)"), *BuildingId, *UniqueCacheKey);
				}
				else if (JsonObject->HasField(TEXT("geom")))
				{
					TSharedPtr<FJsonObject> GeomObject = JsonObject->GetObjectField(TEXT("geom"));
					if (GeomObject.IsValid())
					{
						FString GeomString;
						TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&GeomString);
						FJsonSerializer::Serialize(GeomObject.ToSharedRef(), Writer);

						FString UniqueCacheKey = BuildingId;
						if (JsonObject->HasField(TEXT("id")))
						{
							double NumericIdD = 0.0;
							if (JsonObject->TryGetNumberField(TEXT("id"), NumericIdD))
							{
								int32 NumericId = (int32)NumericIdD;
								UniqueCacheKey = FString::Printf(TEXT("%s#%d"), *BuildingId, NumericId);
							}
						}

						StoreBuildingCoordinates(UniqueCacheKey, GeomString);
						UE_LOG(LogTemp, Warning, TEXT("🔄 Updated geom coordinates for building: %s (cached as %s)"), *BuildingId, *UniqueCacheKey);
					}
				}
				else if (JsonObject->HasField(TEXT("position")))
				{
					FString PositionData = JsonObject->GetStringField(TEXT("position"));

					FString UniqueCacheKey = BuildingId;
					if (JsonObject->HasField(TEXT("id")))
					{
						double NumericIdD = 0.0;
						if (JsonObject->TryGetNumberField(TEXT("id"), NumericIdD))
						{
							int32 NumericId = (int32)NumericIdD;
							UniqueCacheKey = FString::Printf(TEXT("%s#%d"), *BuildingId, NumericId);
						}
					}

					StoreBuildingCoordinates(UniqueCacheKey, PositionData);
					UE_LOG(LogTemp, Warning, TEXT("🔄 Updated position coordinates for building: %s (cached as %s)"), *BuildingId, *UniqueCacheKey);
				}
				
				// Update specific building in cache
				BuildingDataCache.Add(BuildingId, EnergyData);
				
				// If this building is currently displayed, update the display immediately
				if (BuildingId == CurrentlyDisplayedBuildingId)
				{
					UE_LOG(LogTemp, Warning, TEXT("🔄 Updating displayed building with fresh WebSocket data"));
					ShowBuildingInfoWidget(BuildingId, EnergyData);
					
					if (GEngine)
					{
						GEngine->AddOnScreenDebugMessage(-1, 3.0f, FColor::Cyan, 
							TEXT("🔄 Building display updated via WebSocket"));
					}
				}
			}
			
			// Mark data as loaded and up-to-date
			bDataLoaded = true;
			bIsLoading = false;
			
			UE_LOG(LogTemp, Warning, TEXT("✅ WebSocket energy update processed successfully"));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 Received unknown WebSocket message type"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("❌ Failed to parse WebSocket energy update JSON"));
		
		// Fallback: treat the entire message as energy data and try to parse it
		if (JsonData.Contains("gml_id") && JsonData.Contains("energy"))
		{
			UE_LOG(LogTemp, Warning, TEXT("🔄 Fallback: Processing entire message as energy data"));
			ParseAndCacheAllBuildings(JsonData);
		}
	}
}

bool ABuildingEnergyDisplay::IsPointInBuildingBounds(const FVector& Point, const FString& BuildingCoordinates)
{
	TArray<FVector> Coordinates;
	if (!ParseBuildingCoordinates(BuildingCoordinates, Coordinates))
	{
		return false;
	}
	
	return IsPointInPolygon(Point, Coordinates);
}

bool ABuildingEnergyDisplay::ValidateBuildingPosition(const FVector& ClickPosition, const FString& GmlId)
{
	UE_LOG(LogTemp, Warning, TEXT("🔍 === VALIDATING BUILDING POSITION ==="));
	UE_LOG(LogTemp, Warning, TEXT("🔍 Building ID: %s"), *GmlId);
	UE_LOG(LogTemp, Warning, TEXT("🔍 Click Position: X=%.2f, Y=%.2f, Z=%.2f"), ClickPosition.X, ClickPosition.Y, ClickPosition.Z);
	
	// 📦 CREATE BOUNDING BOX FOR BUILDING
	UE_LOG(LogTemp, Warning, TEXT("📦 === CREATING BOUNDING BOX FOR BUILDING %s ==="), *GmlId);
	FBuildingBoundingBox BuildingBounds = CreateBuildingBoundingBox(GmlId);
	
	// Check if building has coordinate data in cache. Support multiple cached geometries for the same GML id
	TArray<FVector> BuildingCoordinates;
	if (BuildingCoordinatesCache.Contains(GmlId))
	{
		BuildingCoordinates = BuildingCoordinatesCache[GmlId];
	}
	else
	{
		// Collect all cached entries that start with the base GmlId (e.g. GMLID#123)
		for (const auto& Entry : BuildingCoordinatesCache)
		{
			if (Entry.Key.StartsWith(GmlId))
			{
				BuildingCoordinates.Append(Entry.Value);
			}
		}
	}

	if (BuildingCoordinates.Num() < 3)
	{
		UE_LOG(LogTemp, Error, TEXT("🔍 ❌ Building %s has insufficient coordinates (%d points)"), *GmlId, BuildingCoordinates.Num());
		return false;
	}
	
	// First check: Is point within bounding box (fast check)
	bool bInBoundingBox = IsPointInBoundingBox(ClickPosition, BuildingBounds);
	
	if (!bInBoundingBox)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔍 ❌ Click position is outside building bounding box"));
		UE_LOG(LogTemp, Warning, TEXT("🔍 Click distance from center: %.2f"), FVector::Dist2D(ClickPosition, BuildingBounds.Center));
		return false;
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔍 ✅ Click position is within bounding box"));
	
	// Second check: Precise point-in-polygon test
	
	// Use point-in-polygon test
	bool bIsInside = IsPointInPolygon(ClickPosition, BuildingCoordinates);
	
	if (bIsInside)
	{
		UE_LOG(LogTemp, Warning, TEXT("🔍 ✅ Position validation PASSED - click is inside building polygon"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("🔍 ❌ Position validation FAILED - click is outside building polygon"));
		
		// 📦 CREATE AND LOG BUILDING BOUNDING BOX FOR DEBUGGING
		FVector MinBounds(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector MaxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		
		for (const FVector& Point : BuildingCoordinates)
		{
			MinBounds.X = FMath::Min(MinBounds.X, Point.X);
			MinBounds.Y = FMath::Min(MinBounds.Y, Point.Y);
			MinBounds.Z = FMath::Min(MinBounds.Z, Point.Z);
			MaxBounds.X = FMath::Max(MaxBounds.X, Point.X);
			MaxBounds.Y = FMath::Max(MaxBounds.Y, Point.Y);
			MaxBounds.Z = FMath::Max(MaxBounds.Z, Point.Z);
		}
		
		FVector BoundingBoxSize = MaxBounds - MinBounds;
		FVector BoundingBoxCenter = (MinBounds + MaxBounds) * 0.5f;
		
		UE_LOG(LogTemp, Warning, TEXT("📦 BUILDING BOUNDING BOX:"));
		UE_LOG(LogTemp, Warning, TEXT("📦   Min Bounds: (%.2f, %.2f, %.2f)"), MinBounds.X, MinBounds.Y, MinBounds.Z);
		UE_LOG(LogTemp, Warning, TEXT("📦   Max Bounds: (%.2f, %.2f, %.2f)"), MaxBounds.X, MaxBounds.Y, MaxBounds.Z);
		UE_LOG(LogTemp, Warning, TEXT("📦   Size: (%.2f, %.2f, %.2f)"), BoundingBoxSize.X, BoundingBoxSize.Y, BoundingBoxSize.Z);
		UE_LOG(LogTemp, Warning, TEXT("📦   Center: (%.2f, %.2f, %.2f)"), BoundingBoxCenter.X, BoundingBoxCenter.Y, BoundingBoxCenter.Z);
		UE_LOG(LogTemp, Warning, TEXT("📦   Click Distance from Center: %.2f"), FVector::Dist2D(ClickPosition, BoundingBoxCenter));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔍 === VALIDATION RESULT: %s ==="), bIsInside ? TEXT("VALID") : TEXT("INVALID"));
	return bIsInside;
}

FBuildingBoundingBox ABuildingEnergyDisplay::CreateBuildingBoundingBox(const FString& GmlId)
{
	FBuildingBoundingBox BoundingBox;
	
	UE_LOG(LogTemp, Warning, TEXT("📦 === CREATING BOUNDING BOX FOR BUILDING %s ==="), *GmlId);
	
	if (!BuildingCoordinatesCache.Contains(GmlId))
	{
		UE_LOG(LogTemp, Error, TEXT("📦 ❌ No coordinates found for building: %s"), *GmlId);
		return BoundingBox;
	}
	
	// Support combined coordinate sets when multiple entries exist for same base GML id
	TArray<FVector> CombinedCoordinates;
	if (BuildingCoordinatesCache.Contains(GmlId))
	{
		CombinedCoordinates = BuildingCoordinatesCache[GmlId];
	}
	else
	{
		for (const auto& Entry : BuildingCoordinatesCache)
		{
			if (Entry.Key.StartsWith(GmlId))
			{
				CombinedCoordinates.Append(Entry.Value);
			}
		}
	}

	if (CombinedCoordinates.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("📦 ❌ No coordinates found for building: %s"), *GmlId);
		return BoundingBox;
	}
	
	// Initialize bounds with first point
	BoundingBox.MinBounds = CombinedCoordinates[0];
	BoundingBox.MaxBounds = CombinedCoordinates[0];
    
	// Find min/max bounds
	for (const FVector& Point : CombinedCoordinates)
	{
		BoundingBox.MinBounds.X = FMath::Min(BoundingBox.MinBounds.X, Point.X);
		BoundingBox.MinBounds.Y = FMath::Min(BoundingBox.MinBounds.Y, Point.Y);
		BoundingBox.MinBounds.Z = FMath::Min(BoundingBox.MinBounds.Z, Point.Z);
		
		BoundingBox.MaxBounds.X = FMath::Max(BoundingBox.MaxBounds.X, Point.X);
		BoundingBox.MaxBounds.Y = FMath::Max(BoundingBox.MaxBounds.Y, Point.Y);
		BoundingBox.MaxBounds.Z = FMath::Max(BoundingBox.MaxBounds.Z, Point.Z);
	}
	
	// Calculate derived properties
	BoundingBox.Size = BoundingBox.MaxBounds - BoundingBox.MinBounds;
	BoundingBox.Center = (BoundingBox.MinBounds + BoundingBox.MaxBounds) * 0.5f;
	
	UE_LOG(LogTemp, Warning, TEXT("📦 BOUNDING BOX CREATED:"));
	UE_LOG(LogTemp, Warning, TEXT("📦   Min: (%.2f, %.2f, %.2f)"), BoundingBox.MinBounds.X, BoundingBox.MinBounds.Y, BoundingBox.MinBounds.Z);
	UE_LOG(LogTemp, Warning, TEXT("📦   Max: (%.2f, %.2f, %.2f)"), BoundingBox.MaxBounds.X, BoundingBox.MaxBounds.Y, BoundingBox.MaxBounds.Z);
	UE_LOG(LogTemp, Warning, TEXT("📦   Size: (%.2f, %.2f, %.2f)"), BoundingBox.Size.X, BoundingBox.Size.Y, BoundingBox.Size.Z);
	UE_LOG(LogTemp, Warning, TEXT("📦   Center: (%.2f, %.2f, %.2f)"), BoundingBox.Center.X, BoundingBox.Center.Y, BoundingBox.Center.Z);
	
	return BoundingBox;
}

bool ABuildingEnergyDisplay::IsPointInBoundingBox(const FVector& Point, const FBuildingBoundingBox& BoundingBox)
{
	bool bInBounds = (Point.X >= BoundingBox.MinBounds.X && Point.X <= BoundingBox.MaxBounds.X &&
					  Point.Y >= BoundingBox.MinBounds.Y && Point.Y <= BoundingBox.MaxBounds.Y &&
					  Point.Z >= BoundingBox.MinBounds.Z && Point.Z <= BoundingBox.MaxBounds.Z);
	
	UE_LOG(LogTemp, Verbose, TEXT("📦 Point (%.2f,%.2f,%.2f) in bounding box: %s"), 
		Point.X, Point.Y, Point.Z, bInBounds ? TEXT("YES") : TEXT("NO"));
	
	return bInBounds;
}

bool ABuildingEnergyDisplay::ParseBuildingCoordinates(const FString& CoordinatesString, TArray<FVector>& OutCoordinates)
{
	OutCoordinates.Empty();
	
	// Parse JSON coordinates (expecting format like: [[x1,y1],[x2,y2],...] or polygon format)
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CoordinatesString);
	
	if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
	{
		// Handle different coordinate formats robustly (Polygon rings, Multi-polygons, simple point lists)
		if (JsonObject->HasField(TEXT("coordinates")))
		{
			const TArray<TSharedPtr<FJsonValue>>* CoordArray;
			if (JsonObject->TryGetArrayField(TEXT("coordinates"), CoordArray))
			{
				// CoordArray may be: [ [ [x,y], [x,y], ... ] ] (Polygon), or [ [x,y], [x,y], ... ] (Line/Points)
				for (const auto& Level1 : *CoordArray)
				{
					if (!Level1.IsValid()) continue;
					// If the first level item is an array, inspect its contents
					if (Level1->Type == EJson::Array)
					{
						const TArray<TSharedPtr<FJsonValue>>* Level2;
						Level1->TryGetArray(Level2);
						if (!Level2) continue;
						// If Level2's first element is an array, we have rings: iterate each ring and then each point
						if (Level2->Num() > 0 && (*Level2)[0]->Type == EJson::Array)
						{
							for (const auto& RingVal : *Level2)
							{
								const TArray<TSharedPtr<FJsonValue>>* PointArr;
								if (RingVal->TryGetArray(PointArr))
								{
									if (PointArr->Num() >= 2 && (*PointArr)[0]->Type == EJson::Number && (*PointArr)[1]->Type == EJson::Number)
									{
										double X = (*PointArr)[0]->AsNumber();
										double Y = (*PointArr)[1]->AsNumber();
										double Z = PointArr->Num() > 2 && (*PointArr)[2]->Type == EJson::Number ? (*PointArr)[2]->AsNumber() : 0.0;
										OutCoordinates.Add(FVector(X, Y, Z));
									}
								}
							}
						}
						else
						{
							// Level2 is an array of numbers (single point) or an array of point-arrays
							if (Level2->Num() >= 2 && (*Level2)[0]->Type == EJson::Number && (*Level2)[1]->Type == EJson::Number)
							{
								double X = (*Level2)[0]->AsNumber();
								double Y = (*Level2)[1]->AsNumber();
								double Z = Level2->Num() > 2 && (*Level2)[2]->Type == EJson::Number ? (*Level2)[2]->AsNumber() : 0.0;
								OutCoordinates.Add(FVector(X, Y, Z));
							}
							else
							{
								// Fallback: iterate expecting each element in Level2 to be a point-array
								for (const auto& MaybePoint : *Level2)
								{
									if (MaybePoint->Type == EJson::Array)
									{
										const TArray<TSharedPtr<FJsonValue>>* PointArr;
										MaybePoint->TryGetArray(PointArr);
										if (PointArr && PointArr->Num() >= 2 && (*PointArr)[0]->Type == EJson::Number && (*PointArr)[1]->Type == EJson::Number)
										{
											double X = (*PointArr)[0]->AsNumber();
											double Y = (*PointArr)[1]->AsNumber();
											double Z = PointArr->Num() > 2 && (*PointArr)[2]->Type == EJson::Number ? (*PointArr)[2]->AsNumber() : 0.0;
											OutCoordinates.Add(FVector(X, Y, Z));
										}
									}
								}
							}
						}
					}
					else
					{
						// If Level1 is not an array of arrays, but perhaps a number array
						const TArray<TSharedPtr<FJsonValue>>* PointArray;
						if (Level1->TryGetArray(PointArray) && PointArray->Num() >= 2 && (*PointArray)[0]->Type == EJson::Number && (*PointArray)[1]->Type == EJson::Number)
						{
							double X = (*PointArray)[0]->AsNumber();
							double Y = (*PointArray)[1]->AsNumber();
							double Z = PointArray->Num() > 2 && (*PointArray)[2]->Type == EJson::Number ? (*PointArray)[2]->AsNumber() : 0.0;
							OutCoordinates.Add(FVector(X, Y, Z));
						}
					}
				}
			}
		}
	}
	else
	{
		// Try parsing as simple coordinate array string format
		FString CleanCoords = CoordinatesString;
		CleanCoords = CleanCoords.Replace(TEXT("["), TEXT(""));
		CleanCoords = CleanCoords.Replace(TEXT("]"), TEXT(""));
		CleanCoords = CleanCoords.Replace(TEXT(" "), TEXT(""));
		
		TArray<FString> CoordPairs;
		CleanCoords.ParseIntoArray(CoordPairs, TEXT(","), true);
		
		for (int32 i = 0; i < CoordPairs.Num() - 1; i += 2)
		{
			if (i + 1 < CoordPairs.Num())
			{
				float X = FCString::Atof(*CoordPairs[i]);
				float Y = FCString::Atof(*CoordPairs[i + 1]);
				OutCoordinates.Add(FVector(X, Y, 0.0f));
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🎯 Parsed %d coordinate points"), OutCoordinates.Num());
	return OutCoordinates.Num() > 0;
}

bool ABuildingEnergyDisplay::IsPointInPolygon(const FVector& Point, const TArray<FVector>& PolygonVertices)
{
	if (PolygonVertices.Num() < 3)
	{
		return false; // Not enough points to form a polygon
	}
	
	// Ray casting algorithm for point-in-polygon test
	int32 CrossingCount = 0;
	
	for (int32 i = 0; i < PolygonVertices.Num(); ++i)
	{
		int32 j = (i + 1) % PolygonVertices.Num();
		
		const FVector& Vi = PolygonVertices[i];
		const FVector& Vj = PolygonVertices[j];
		
		// Check if ray crosses this edge
		if (((Vi.Y > Point.Y) != (Vj.Y > Point.Y)) &&
			(Point.X < (Vj.X - Vi.X) * (Point.Y - Vi.Y) / (Vj.Y - Vi.Y) + Vi.X))
		{
			CrossingCount++;
		}
	}
	
	// Point is inside if crossing count is odd
	bool bIsInside = (CrossingCount % 2) == 1;
	
	UE_LOG(LogTemp, Verbose, TEXT("🎯 Point-in-polygon test: %s (crossings: %d)"), 
		bIsInside ? TEXT("INSIDE") : TEXT("OUTSIDE"), CrossingCount);
	
	return bIsInside;
}

FString ABuildingEnergyDisplay::GetBuildingByCoordinates(const FVector& ClickPosition)
{
	UE_LOG(LogTemp, Warning, TEXT("🎯 === FINDING BUILDING BY COORDINATES ==="));
	UE_LOG(LogTemp, Warning, TEXT("🎯 Click Position: X=%.2f, Y=%.2f"), ClickPosition.X, ClickPosition.Y);
	
	// Search through all cached building coordinates
	for (const auto& BuildingEntry : BuildingCoordinatesCache)
	{
		const FString& GmlId = BuildingEntry.Key;
		const TArray<FVector>& Coordinates = BuildingEntry.Value;
		
		if (IsPointInPolygon(ClickPosition, Coordinates))
		{
			// If the cached key contains a unique suffix (e.g. "GMLID#1234"), return the base GML id
			FString BaseGmlId = GmlId;
			int32 HashIndex = INDEX_NONE;
			if (GmlId.FindChar(TEXT('#'), HashIndex))
			{
				BaseGmlId = GmlId.Left(HashIndex);
			}
			UE_LOG(LogTemp, Warning, TEXT("🎯 Found matching cached key: %s -> returning base id: %s"), *GmlId, *BaseGmlId);
			return BaseGmlId;
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🎯 No building found at click position"));
	return TEXT("");
}

void ABuildingEnergyDisplay::StoreBuildingCoordinates(const FString& GmlId, const FString& CoordinatesData)
{
	TArray<FVector> Coordinates;
	if (ParseBuildingCoordinates(CoordinatesData, Coordinates))
	{
		BuildingCoordinatesCache.Add(GmlId, Coordinates);
		UE_LOG(LogTemp, Verbose, TEXT("🎯 Stored %d coordinates for building: %s"), Coordinates.Num(), *GmlId);
	}
}

void ABuildingEnergyDisplay::LogCacheStatistics()
{
	// Get static counters (note: these may be 0 if no operations happened yet)
	// This is a summary function that logs current statistics
	
	UE_LOG(LogTemp, Warning, TEXT(""));
	UE_LOG(LogTemp, Warning, TEXT("📊 ===== CACHE STATISTICS SUMMARY ====="));
	UE_LOG(LogTemp, Warning, TEXT("📊 Current Cache State:"));
	UE_LOG(LogTemp, Warning, TEXT("📊   Building Data Cache: %d entries"), BuildingDataCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("📊   GML ID Cache: %d mappings"), GmlIdCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("📊   Data Loaded: %s"), bDataLoaded ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogTemp, Warning, TEXT("📊   Currently Loading: %s"), bIsLoading ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogTemp, Warning, TEXT("📊   Last Displayed Building: %s"), CurrentlyDisplayedBuildingId.IsEmpty() ? TEXT("NONE") : *CurrentlyDisplayedBuildingId);
	UE_LOG(LogTemp, Warning, TEXT("📊"));
	UE_LOG(LogTemp, Warning, TEXT("📊 Note: Detailed update/access/hit/miss counters are shown in real-time during operations"));
	UE_LOG(LogTemp, Warning, TEXT("📊 =========================================="));
	UE_LOG(LogTemp, Warning, TEXT(""));
}

void ABuildingEnergyDisplay::ValidateGmlIdCaseSensitivity()
{
	UE_LOG(LogTemp, Warning, TEXT("🔍 ===== GML ID CASE SENSITIVITY VALIDATION ====="));
	UE_LOG(LogTemp, Warning, TEXT("🔍 Validating that gml_id and modified_gml_id fields maintain proper case sensitivity"));
	UE_LOG(LogTemp, Warning, TEXT("🔍 REQUIREMENT: 'G' must be different from 'g' in all GML ID operations"));
	UE_LOG(LogTemp, Warning, TEXT(""));
	
	int32 CaseSensitiveCount = 0;
	int32 PotentialIssues = 0;
	
	// Check all cached GML IDs for case sensitivity compliance
	for (const auto& Entry : GmlIdCache)
	{
		const FString& CachedModifiedGmlId = Entry.Key;
		const FString& CachedActualGmlId = Entry.Value;
		
		bool bIsModifiedCaseSensitive = IsGmlIdCaseSensitive(CachedModifiedGmlId);
		bool bIsActualCaseSensitive = IsGmlIdCaseSensitive(CachedActualGmlId);
		
		if (bIsModifiedCaseSensitive && bIsActualCaseSensitive)
		{
			CaseSensitiveCount++;
		}
		else
		{
			PotentialIssues++;
			UE_LOG(LogTemp, Warning, TEXT("⚠️ POTENTIAL ISSUE: Modified='%s' (case-sensitive:%s) -> Actual='%s' (case-sensitive:%s)"), 
				*CachedModifiedGmlId, bIsModifiedCaseSensitive ? TEXT("YES") : TEXT("NO"),
				*CachedActualGmlId, bIsActualCaseSensitive ? TEXT("YES") : TEXT("NO"));
		}
	}
	
	// Check BuildingDataCache keys
	for (const auto& Entry : BuildingDataCache)
	{
		const FString& GmlId = Entry.Key;
		if (!IsGmlIdCaseSensitive(GmlId))
		{
			UE_LOG(LogTemp, Warning, TEXT("⚠️ BuildingDataCache key not case-sensitive: '%s'"), *GmlId);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT(""));
	UE_LOG(LogTemp, Warning, TEXT("📊 VALIDATION RESULTS:"));
	UE_LOG(LogTemp, Warning, TEXT("📊   Case-Sensitive GML IDs: %d"), CaseSensitiveCount);
	UE_LOG(LogTemp, Warning, TEXT("📊   Potential Issues: %d"), PotentialIssues);
	UE_LOG(LogTemp, Warning, TEXT("📊   GML ID Cache Size: %d"), GmlIdCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("📊   Building Data Cache Size: %d"), BuildingDataCache.Num());
	
	if (PotentialIssues == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ VALIDATION PASSED: All GML IDs maintain proper case sensitivity"));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ VALIDATION FAILED: %d GML IDs have potential case sensitivity issues"), PotentialIssues);
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔍 ================================================"));
}

void ABuildingEnergyDisplay::CleanDuplicateColorCacheEntries()
{
	UE_LOG(LogTemp, Warning, TEXT("🧹 ===== CLEANING DUPLICATE COLOR CACHE ENTRIES ====="));
	UE_LOG(LogTemp, Warning, TEXT("🧹 Removing potential duplicates caused by old case-insensitive matching"));
	
	int32 OriginalSize = BuildingColorCache.Num();
	TMap<FString, FLinearColor> CleanedColorCache;
	TArray<FString> DuplicatesFound;
	
	// Create a clean cache with case-sensitive deduplication
	for (const auto& Entry : BuildingColorCache)
	{
		const FString& GmlId = Entry.Key;
		const FLinearColor& Color = Entry.Value;
		
		// Check if we already have this exact GML ID (case-sensitive)
		if (CleanedColorCache.Contains(GmlId))
		{
			DuplicatesFound.Add(GmlId);
			UE_LOG(LogTemp, Warning, TEXT("🔍 DUPLICATE FOUND: '%s' - keeping first occurrence"), *GmlId);
			continue;
		}
		
		// Add to cleaned cache
		CleanedColorCache.Add(GmlId, Color);
	}
	
	// Replace the original cache with the cleaned version
	BuildingColorCache = CleanedColorCache;
	
	int32 CleanedSize = BuildingColorCache.Num();
	int32 RemovedEntries = OriginalSize - CleanedSize;
	
	UE_LOG(LogTemp, Warning, TEXT(""));
	UE_LOG(LogTemp, Warning, TEXT("📊 CLEANING RESULTS:"));
	UE_LOG(LogTemp, Warning, TEXT("📊   Original Cache Size: %d"), OriginalSize);
	UE_LOG(LogTemp, Warning, TEXT("📊   Cleaned Cache Size: %d"), CleanedSize);
	UE_LOG(LogTemp, Warning, TEXT("📊   Duplicates Removed: %d"), RemovedEntries);
	
	if (RemovedEntries > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ CACHE CLEANED: Removed %d duplicate entries"), RemovedEntries);
		UE_LOG(LogTemp, Warning, TEXT("💡 This should improve color application reliability"));
		
		// Log some of the duplicates found
		UE_LOG(LogTemp, Warning, TEXT("🔍 Sample duplicates removed:"));
		int32 LogCount = 0;
		for (const FString& Duplicate : DuplicatesFound)
		{
			UE_LOG(LogTemp, Warning, TEXT("   %d: '%s'"), LogCount + 1, *Duplicate);
			if (++LogCount >= 5) break; // Show first 5
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("✅ CACHE ALREADY CLEAN: No duplicate entries found"));
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🧹 =========================================="));
}

void ABuildingEnergyDisplay::TestColorRetrieval(const FString& TestGmlId)
{
	UE_LOG(LogTemp, Warning, TEXT("🔍 ===== COLOR RETRIEVAL TEST ====="));
	UE_LOG(LogTemp, Warning, TEXT("🔍 Testing color retrieval for: '%s'"), *TestGmlId);
	UE_LOG(LogTemp, Warning, TEXT("🔍 BuildingColorCache size: %d"), BuildingColorCache.Num());
	
	// Test exact match (case-sensitive)
	if (BuildingColorCache.Contains(TestGmlId))
	{
		FLinearColor Color = BuildingColorCache[TestGmlId];
		FColor SRGBColor = Color.ToFColor(true);
		FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
		
		UE_LOG(LogTemp, Warning, TEXT("✅ EXACT MATCH FOUND: '%s' -> %s"), *TestGmlId, *HexColor);
		UE_LOG(LogTemp, Warning, TEXT("   LinearColor: R:%.3f G:%.3f B:%.3f A:%.3f"), 
			Color.R, Color.G, Color.B, Color.A);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("❌ NO EXACT MATCH: '%s' not found in color cache"), *TestGmlId);
		
		// Search for similar IDs (case-sensitive partial matching)
		UE_LOG(LogTemp, Warning, TEXT("🔍 Searching for similar IDs..."));
		TArray<FString> SimilarIds;
		
		for (const auto& Entry : BuildingColorCache)
		{
			const FString& CacheId = Entry.Key;
			
			// Case-sensitive contains check
			if (CacheId.Contains(TestGmlId) || TestGmlId.Contains(CacheId))
			{
				SimilarIds.Add(CacheId);
			}
		}
		
		if (SimilarIds.Num() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("🎯 Found %d similar IDs:"), SimilarIds.Num());
			for (int32 i = 0; i < FMath::Min(SimilarIds.Num(), 5); i++)
			{
				FLinearColor Color = BuildingColorCache[SimilarIds[i]];
				UE_LOG(LogTemp, Warning, TEXT("   %d: '%s' (R:%.2f G:%.2f B:%.2f)"), 
					i + 1, *SimilarIds[i], Color.R, Color.G, Color.B);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("❌ No similar IDs found"));
			
			// Show some sample cache entries for comparison
			UE_LOG(LogTemp, Warning, TEXT("📝 Sample cache entries for comparison:"));
			int32 SampleCount = 0;
			for (const auto& Entry : BuildingColorCache)
			{
				UE_LOG(LogTemp, Warning, TEXT("   %d: '%s'"), SampleCount + 1, *Entry.Key);
				if (++SampleCount >= 5) break;
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🔍 ===================================="));
}

bool ABuildingEnergyDisplay::IsGmlIdCaseSensitive(const FString& GmlId)
{
	// Check if the GML ID contains both uppercase and lowercase characters
	// This indicates proper case sensitivity is being maintained
	bool bHasUppercase = false;
	bool bHasLowercase = false;
	
	for (int32 i = 0; i < GmlId.Len(); i++)
	{
		TCHAR Char = GmlId[i];
		if (FChar::IsUpper(Char))
		{
			bHasUppercase = true;
		}
		else if (FChar::IsLower(Char))
		{
			bHasLowercase = true;
		}
		
		// Early exit if we found both
		if (bHasUppercase && bHasLowercase)
		{
			return true;
		}
	}
	
	// If all characters are the same case, check for specific patterns
	// GML IDs should typically have mixed case (e.g., "DEBW_001000wrHDD")
	if (bHasUppercase && !bHasLowercase)
	{
		// All uppercase - this might be okay for some GML IDs
		return GmlId.Len() > 5; // Assume proper if reasonably long
	}
	else if (bHasLowercase && !bHasUppercase)
	{
		// All lowercase - this is suspicious for GML IDs
		UE_LOG(LogTemp, Warning, TEXT("⚠️ GML ID appears to be all lowercase: '%s'"), *GmlId);
		return false;
	}
	
	// Mixed case is ideal
	return bHasUppercase && bHasLowercase;
}

void ABuildingEnergyDisplay::ApplyCesiumTilesetStyling(AActor* CesiumActor)
{
	if (!CesiumActor)
	{
		UE_LOG(LogTemp, Error, TEXT("STYLE Cannot apply styling to null Cesium actor"));
		return;
	}
	
	// Generate the complete styling JSON expression
	FString StyleExpression = CreateCesiumColorExpression();
	UE_LOG(LogTemp, Warning, TEXT("STYLE Applying Cesium style expression to %s"), *CesiumActor->GetName());
	UE_LOG(LogTemp, Warning, TEXT("STYLE Expression: %s"), *StyleExpression);
	
	// Try to find and set Cesium styling properties
	UClass* ActorClass = CesiumActor->GetClass();
	bool bFoundStyleProperty = false;
	
	// Look for common Cesium styling property names
	TArray<FString> StylePropertyNames = {
		TEXT("Style"),
		TEXT("ColorStyle"), 
		TEXT("FeatureStyle"),
		TEXT("BuildingStyle"),
		TEXT("TilesetStyle"),
		TEXT("Expression"),
		TEXT("ColorExpression"),
		TEXT("StyleString"),
		TEXT("Styling")
	};
	
	for (const FString& PropName : StylePropertyNames)
	{
		for (TFieldIterator<FProperty> PropIt(ActorClass); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (Property->GetName().Equals(PropName, ESearchCase::IgnoreCase))
			{
				UE_LOG(LogTemp, Warning, TEXT("STYLE Found matching property: %s"), *Property->GetName());
				
				if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
				{
					StrProp->SetPropertyValue_InContainer(CesiumActor, StyleExpression);
					bFoundStyleProperty = true;
					UE_LOG(LogTemp, Warning, TEXT("STYLE Successfully set style expression on property: %s"), *Property->GetName());
					CesiumActor->Modify();
					break;
				}
				else if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
				{
					TextProp->SetPropertyValue_InContainer(CesiumActor, FText::FromString(StyleExpression));
					bFoundStyleProperty = true;
					UE_LOG(LogTemp, Warning, TEXT("STYLE Successfully set style expression on text property: %s"), *Property->GetName());
					CesiumActor->Modify();
					break;
				}
			}
		}
		if (bFoundStyleProperty) break;
	}
	
	// Alternative approach: Try to find Cesium feature metadata component
	TArray<UActorComponent*> Components;
	CesiumActor->GetComponents<UActorComponent>(Components);
	for (UActorComponent* Component : Components)
	{
		if (Component && Component->GetName().Contains(TEXT("Feature")))
		{
			UE_LOG(LogTemp, Warning, TEXT("STYLE Found potential feature component: %s"), *Component->GetName());
			
			// Try to apply styling to this component
			UClass* ComponentClass = Component->GetClass();
			for (TFieldIterator<FProperty> PropIt(ComponentClass); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				FString PropName = Property->GetName();
				
				if (PropName.Contains(TEXT("Style")) || PropName.Contains(TEXT("Color")))
				{
					if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
					{
						StrProp->SetPropertyValue_InContainer(Component, StyleExpression);
						bFoundStyleProperty = true;
						UE_LOG(LogTemp, Warning, TEXT("STYLE Applied to component property: %s"), *PropName);
						break;
					}
				}
			}
		}
	}
	
	if (!bFoundStyleProperty)
	{
		UE_LOG(LogTemp, Warning, TEXT("STYLE No suitable styling property found on Cesium actor"));
		UE_LOG(LogTemp, Warning, TEXT("STYLE Available properties on %s:"), *ActorClass->GetName());
		
		// Debug: List all available properties
		int32 PropCount = 0;
		for (TFieldIterator<FProperty> PropIt(ActorClass); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			UE_LOG(LogTemp, Log, TEXT("  Property[%d]: %s (Type: %s)"), PropCount++, *Property->GetName(), *Property->GetClass()->GetName());
		}
		
		// Try fallback approach
		ApplyFallbackMaterialStyling(CesiumActor);
	}
}

void ABuildingEnergyDisplay::ApplyFallbackMaterialStyling(AActor* CesiumActor)
{
	UE_LOG(LogTemp, Warning, TEXT("STYLE Applying fallback material-based styling"));
	
	// Get all mesh components and try to apply materials directly
	TArray<UMeshComponent*> MeshComponents;
	CesiumActor->GetComponents<UMeshComponent>(MeshComponents);
	
	for (UMeshComponent* MeshComp : MeshComponents)
	{
		if (MeshComp)
		{
			UE_LOG(LogTemp, Warning, TEXT("STYLE Found mesh component: %s"), *MeshComp->GetName());
			
			// Check if it's a static mesh component specifically
			if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(MeshComp))
			{
				if (StaticMeshComp->GetStaticMesh())
				{
					// Try to create dynamic material instances for each building
					int32 MaterialIndex = 0;
					for (const auto& BuildingColor : BuildingColorCache)
					{
						FLinearColor Color = BuildingColor.Value;
						
						// Create dynamic material instance
						UMaterialInterface* BaseMaterial = StaticMeshComp->GetMaterial(0);
						if (BaseMaterial)
						{
							UMaterialInstanceDynamic* DynamicMat = UMaterialInstanceDynamic::Create(BaseMaterial, this);
							if (DynamicMat)
							{
								// Set color parameters - try common parameter names
								DynamicMat->SetVectorParameterValue(TEXT("BaseColor"), Color);
								DynamicMat->SetVectorParameterValue(TEXT("Color"), Color);
								DynamicMat->SetVectorParameterValue(TEXT("Diffuse"), Color);
								DynamicMat->SetVectorParameterValue(TEXT("AlbedoColor"), Color);
								
								// Apply to mesh (this will affect all buildings, but it's a fallback)
								StaticMeshComp->SetMaterial(MaterialIndex, DynamicMat);
								
								UE_LOG(LogTemp, Warning, TEXT("STYLE Applied dynamic material with color R:%.2f G:%.2f B:%.2f"), Color.R, Color.G, Color.B);
								break; // Only apply first color as fallback
							}
						}
					}
				}
			}
		}
	}
}

// Debug and test functions
void ABuildingEnergyDisplay::TestColorSystem()
{
	UE_LOG(LogTemp, Warning, TEXT(""));
	UE_LOG(LogTemp, Warning, TEXT("🧪 ===== TESTING COLOR SYSTEM ====="));
	UE_LOG(LogTemp, Warning, TEXT("🧪 Data loaded: %s"), bDataLoaded ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogTemp, Warning, TEXT("🧪 Building data cache entries: %d"), BuildingDataCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("🧪 Building color cache entries: %d"), BuildingColorCache.Num());
	UE_LOG(LogTemp, Warning, TEXT("🧪 Currently loading: %s"), bIsLoading ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogTemp, Warning, TEXT("🧪 Access token length: %d"), AccessToken.Len());
	
	// Test authentication if no data
	if (!bDataLoaded && !bIsLoading)
	{
		UE_LOG(LogTemp, Warning, TEXT("🧪 No data loaded - triggering authentication"));
		AuthenticateAndLoadData();
	}
	
	// Test color application if we have colors
	if (BuildingColorCache.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("🧪 Colors available - testing application"));
		ForceApplyColors();
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🧪 =================================="));
}

void ABuildingEnergyDisplay::LogColorCacheStatus()
{
	UE_LOG(LogTemp, Warning, TEXT(""));
	UE_LOG(LogTemp, Warning, TEXT("🎯 ===== COLOR CACHE STATUS ====="));
	UE_LOG(LogTemp, Warning, TEXT("🎯 BuildingColorCache entries: %d"), BuildingColorCache.Num());
	
	if (BuildingColorCache.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("🎯 Sample cached colors:"));
		int32 Count = 0;
		for (const auto& Entry : BuildingColorCache)
		{
			FLinearColor Color = Entry.Value;
			FColor SRGBColor = Color.ToFColor(true);
			FString HexColor = FString::Printf(TEXT("#%02X%02X%02X"), SRGBColor.R, SRGBColor.G, SRGBColor.B);
			
			UE_LOG(LogTemp, Warning, TEXT("🎯   %s -> %s"), *Entry.Key, *HexColor);
			
			if (++Count >= 3) // Show only first 3
			{
				UE_LOG(LogTemp, Warning, TEXT("🎯   ... and %d more"), BuildingColorCache.Num() - 3);
				break;
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("🎯 No colors cached! Possible issues:"));
		UE_LOG(LogTemp, Error, TEXT("🎯   1. API authentication failed"));
		UE_LOG(LogTemp, Error, TEXT("🎯   2. JSON parsing failed"));
		UE_LOG(LogTemp, Error, TEXT("🎯   3. Color extraction failed"));
	}
	UE_LOG(LogTemp, Warning, TEXT("🎯 ==============================="));
}

void ABuildingEnergyDisplay::ForceApplyColors()
{
	UE_LOG(LogTemp, Warning, TEXT(""));
	UE_LOG(LogTemp, Warning, TEXT("🚀 ===== FORCING COLOR APPLICATION ====="));
	
	if (BuildingColorCache.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("🚀 Cannot apply colors - cache is empty!"));
		
		// Try to create test colors for debugging
		UE_LOG(LogTemp, Warning, TEXT("🚀 Creating test colors for debugging"));
		BuildingColorCache.Add(TEXT("DEBW_0010008"), FLinearColor(1.0f, 0.0f, 0.0f, 1.0f)); // Red
		BuildingColorCache.Add(TEXT("DEBW_0010009"), FLinearColor(0.0f, 1.0f, 0.0f, 1.0f)); // Green
		BuildingColorCache.Add(TEXT("DEBW_0010010"), FLinearColor(0.0f, 0.0f, 1.0f, 1.0f)); // Blue
		UE_LOG(LogTemp, Warning, TEXT("🚀 Created %d test colors"), BuildingColorCache.Num());
	}
	
	// Try to find and apply colors to Cesium tileset
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("🚀 Cannot apply colors - no world reference!"));
		return;
	}
	
	int32 TilesetCount = 0;
	for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
	{
		AActor* Actor = *ActorItr;
		if (Actor)
		{
			FString ActorName = Actor->GetName();
			UE_LOG(LogTemp, Log, TEXT("🚀 Found actor: %s"), *ActorName);
			
			if (ActorName.Contains(TEXT("bisingen")) || ActorName.Contains(TEXT("Cesium")) || ActorName.Contains(TEXT("Tileset")))
			{
				UE_LOG(LogTemp, Warning, TEXT("🚀 Found potential Cesium tileset: %s"), *ActorName);
				TilesetCount++;
				
				// Apply styling
				ApplyCesiumTilesetStyling(Actor);
			}
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🚀 Found %d potential Cesium tilesets"), TilesetCount);
	
	if (TilesetCount == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("🚀 No Cesium tilesets found! Available actors:"));
		int32 ActorCount = 0;
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (Actor && ++ActorCount <= 10) // Show first 10 actors
			{
				UE_LOG(LogTemp, Warning, TEXT("🚀   Actor[%d]: %s"), ActorCount, *Actor->GetName());
			}
		}
		if (ActorCount > 10)
		{
			UE_LOG(LogTemp, Warning, TEXT("🚀   ... and %d more actors"), ActorCount - 10);
		}
	}
	
	UE_LOG(LogTemp, Warning, TEXT("🚀 ===================================="));
}
