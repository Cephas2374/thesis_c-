# Real-Time Building Energy Visualization in Unreal Engine: Technical Methodology

## Abstract

This methodology presents a comprehensive approach for implementing real-time building energy visualization within Unreal Engine 5.6 using Cesium for 3D geospatial data representation. The system integrates a RESTful backend API with Unreal Engine's C++ framework to provide dynamic, per-building energy consumption visualization through color-coded 3D tilesets. The approach addresses challenges in real-time data synchronization, robust JSON parsing, coordinate mapping, and Cesium tileset styling within the Unreal Engine ecosystem.

## 1. Architectural Design and Data Flow

### 1.1 System Architecture Overview

The proposed system employs a three-tier architecture consisting of a backend data service, an Unreal Engine client application, and a Cesium 3D tileset rendering layer. The backend service, hosted at `backend.gisworld-tech.com`, provides RESTful endpoints for building energy data retrieval and modification. The Unreal Engine client implements a custom C++ actor class (`ABuildingEnergyDisplay`) that manages data synchronization, coordinate mapping, and visual representation through Cesium's 3D tileset infrastructure.

The data flow follows a polling-based approach where the Unreal Engine client periodically queries the backend API for building energy updates. This design decision was made to ensure reliability and eliminate the complexities associated with WebSocket connection management in the Unreal Engine environment. The polling mechanism implements exponential backoff strategies and connection pooling to optimize network resource utilization while maintaining real-time responsiveness.

### 1.2 Authentication and Security Framework

The system implements JWT (JSON Web Token) based authentication with automatic token refresh capabilities. The authentication flow utilizes both access tokens for API requests and refresh tokens for seamless session management. The implementation handles token expiration gracefully by intercepting HTTP 401 responses and automatically attempting token refresh before retrying the original request. This approach ensures uninterrupted data flow while maintaining security standards.

Token storage and management are handled entirely in memory within the Unreal Engine client, eliminating persistent storage security concerns. The authentication state machine manages token lifecycle events, including initial authentication, automatic refresh, and error recovery scenarios. Security considerations include proper token validation, secure transmission over HTTPS, and implementation of request rate limiting to prevent abuse.

### 1.3 Data Synchronization Strategy

The synchronization strategy addresses the challenge of maintaining consistency between the backend data state and the client-side visualization. The system implements a multi-layered caching mechanism that includes building metadata cache, coordinate mapping cache, and color assignment cache. This approach minimizes network requests while ensuring data freshness through intelligent cache invalidation strategies.

The cache architecture employs multiple key mappings to handle the complexity of building identification systems. Buildings are indexed by both `modified_gml_id` and `gml_id` identifiers, with additional coordinate-based lookup capabilities for spatial queries. The coordinate caching system handles duplicate GML IDs by implementing unique cache keys that incorporate geometry-specific identifiers, ensuring that buildings with identical IDs but different geometric representations are properly distinguished.

## 2. Geospatial Data Processing and Coordinate System Management

### 2.1 GeoJSON Processing Pipeline

The system implements a robust GeoJSON processing pipeline capable of handling complex geometric structures including nested polygon arrays, multi-polygon geometries, and coordinate ring extraction. The processing algorithm employs defensive programming techniques with comprehensive type checking to prevent runtime errors when encountering unexpected data structures.

The coordinate extraction process handles multiple geometric representation formats commonly found in building datasets. The algorithm recursively traverses nested coordinate arrays, extracting individual coordinate pairs while maintaining spatial relationship integrity. Error handling mechanisms ensure graceful degradation when encountering malformed geometric data, logging warnings while continuing processing with available valid coordinates.

Coordinate system transformation is managed through Unreal Engine's native coordinate system, with proper scaling and offset calculations to ensure accurate spatial representation. The system handles the conversion between geographic coordinate systems (WGS84) and Unreal Engine's local coordinate space, maintaining spatial accuracy for visualization purposes.

### 2.2 Building Identification and Mapping

The building identification system addresses the complexity of managing multiple identifier formats within the same dataset. The implementation handles mapping between `modified_gml_id` and `gml_id` formats, where identifiers may differ by underscore versus 'L' character substitutions (e.g., `DEBW_0010008` vs `DEBWL0010008`). This mapping is crucial for ensuring proper correspondence between API data and Cesium tileset feature metadata.

The mapping algorithm implements bidirectional lookup capabilities, allowing queries by either identifier format. Additional complexity arises from buildings with identical GML IDs but different geometric representations, which the system addresses through unique cache key generation that incorporates geometry-specific identifiers when available.

Spatial indexing capabilities enable coordinate-based building identification, supporting use cases where building selection occurs through 3D scene interaction rather than direct identifier lookup. The spatial matching algorithm implements tolerance-based coordinate comparison to handle floating-point precision variations inherent in coordinate data processing.

## 3. Energy Data Integration and Visualization

### 3.1 Energy Data Model and Processing

The energy data model supports complex temporal energy consumption patterns with multiple time series data points per building. The API response structure includes nested energy result objects with temporal ranges (begin/end periods) and associated energy consumption values. Each time period includes color assignments for visualization purposes, typically encoded as hexadecimal color values.

The data processing pipeline extracts energy consumption values and associated metadata while handling missing or null data gracefully. The algorithm implements fallback strategies for incomplete data sets, ensuring that partial information doesn't prevent visualization of available data. Color extraction handles multiple color encoding formats and provides default color assignment when energy-specific colors are unavailable.

Data validation ensures that extracted energy values fall within expected ranges and that color values conform to valid hexadecimal color specifications. The validation process logs warnings for anomalous data while maintaining system stability through defensive programming practices.

### 3.2 Color Mapping and Visual Representation

The color mapping system converts energy consumption data into visual representations suitable for 3D tileset rendering. The implementation supports both direct color assignment from API data and algorithmic color generation based on energy consumption ranges. The color conversion process handles multiple color space representations, including hexadecimal, RGB, and Unreal Engine's native FLinearColor format.

The visual representation strategy employs Cesium's feature-based styling capabilities to achieve per-building color assignment. The implementation generates Cesium-compatible style expressions that match building identifiers to color values, creating conditional styling rules that the Cesium rendering engine can interpret and apply during tileset rendering.

Alternative visualization approaches are implemented as fallback mechanisms, including material-based coloring for scenarios where Cesium feature styling is unavailable. The material-based approach applies color assignments through Unreal Engine's material system, providing a backup visualization method that ensures color representation regardless of Cesium feature support availability.

## 4. Real-Time User Interaction and Data Modification

### 4.1 Interactive Building Selection and Attribute Display

The interactive system enables real-time building selection through 3D scene interaction, triggering the display of detailed building attribute information. The selection mechanism implements ray casting from user input to identify the selected building within the 3D tileset, retrieving the corresponding building identifier for data lookup operations.

The attribute display system presents building information through Unreal Engine's UMG (Unreal Motion Graphics) interface, providing a responsive user interface for data visualization and modification. The interface design supports both read-only data display and interactive form controls for attribute modification, with real-time validation and feedback mechanisms.

**Blueprint Architecture and User Interface Implementation**: The system employs a hybrid C++/Blueprint architecture where core functionality is implemented in C++ for performance-critical operations, while user interface logic and event handling utilize Unreal Engine's Blueprint visual scripting system. The Blueprint implementation manages the interaction between the building selection system and the UMG interface, handling events such as building clicks, form submissions, and real-time data updates. The UMG form design incorporates responsive layout principles suitable for both desktop and mixed reality deployment, with input field validation, dropdown selections for building attributes, and dynamic content updates based on selected building data.

The system maintains state consistency between the 3D visualization and the attribute display interface, ensuring that changes in one component are immediately reflected in related components. This approach provides a seamless user experience while maintaining data integrity across the application.

### 4.2 Software and Development Tools

The development methodology employs a comprehensive suite of industry-standard software tools and development environments optimized for cross-platform mixed reality application development. **Unreal Engine 5.6** serves as the primary development platform, providing the foundational framework for 3D visualization, cross-platform deployment capabilities, and mixed reality support through its integrated Windows Mixed Reality subsystem. The engine's Blueprint visual scripting system is complemented by extensive C++ programming to achieve the performance and functionality requirements necessary for real-time building energy visualization.

**Visual Studio 2022 Community Edition** functions as the integrated development environment (IDE), offering comprehensive debugging capabilities, IntelliSense code completion, and seamless integration with Unreal Engine's build system. The IDE's mixed-mode debugging capabilities prove essential for troubleshooting both C++ backend logic and Blueprint visual scripting components simultaneously. **MSBuild** handles the complex compilation requirements, managing dependencies between Unreal Engine modules, third-party libraries, and custom C++ classes while supporting both development and shipping build configurations.

The **Cesium for Unreal plugin** provides the core geospatial visualization capabilities, enabling integration of 3D building tilesets with real-time energy data overlay systems. This plugin handles coordinate system transformations, LOD (Level of Detail) management for large datasets, and feature-based styling essential for per-building color assignment. **Git version control** manages source code versioning and collaboration, with the repository structure accommodating both Unreal Engine project files and custom C++ source code while maintaining appropriate ignore patterns for generated content.

**Postman** serves as the API testing and development tool, enabling rapid prototyping and validation of REST endpoint interactions during authentication and data retrieval development phases. The tool's collection and environment management features facilitate testing across different deployment scenarios and authentication states. **HoloLens 2 Device Portal** and **Windows Device Portal** provide remote debugging and deployment capabilities for mixed reality testing, enabling wireless deployment and real-time performance monitoring during HoloLens development iterations.

### 4.3 System Workflow and Data Pipeline

The comprehensive workflow diagram illustrates the end-to-end data pipeline from initial building energy data acquisition through final mixed reality visualization. The process begins with **data acquisition and enrichment** where building geometries from CityGML standards are combined with real-time energy consumption data from the STEG REST API. This enriched dataset flows into a **backend infrastructure** comprising Django REST framework with PostgreSQL spatial database storage, enabling efficient querying and manipulation of both geometric and energy attribute data.

The **3D tileset generation** phase employs Geodan's pg2b3dm tool to convert the enriched building data into Cesium-compatible 3D tiles, which are then hosted through **Cesium Ion** for global accessibility and optimized streaming. The **Unreal Engine client application** consumes these tilesets alongside real-time energy data through authenticated API calls, implementing the C++/Blueprint hybrid architecture detailed in previous sections. The **cross-platform deployment** capability enables the same codebase to target both desktop environments and **HoloLens 2 mixed reality devices** through AR SDK integration (ARKit/OpenXR).

The workflow culminates in **user interaction scenarios** where stakeholders can visualize building energy performance in immersive 3D environments, perform real-time data edits through spatial interfaces, and leverage mixed reality capabilities for on-site building energy assessment. This complete pipeline demonstrates the integration of geospatial data standards, modern web technologies, game engine capabilities, and emerging mixed reality platforms to create a comprehensive building energy visualization ecosystem.

### 4.4 Data Quality and Sources

The research implementation utilizes **Bisingen, Germany** as the primary case study location, providing a representative European municipal environment for validating the building energy visualization methodology. Bisingen, located in Baden-Württemberg, offers an ideal testbed with its mixed residential and commercial building portfolio typical of mid-sized German municipalities, enabling comprehensive evaluation of the system's performance across diverse building types and energy consumption patterns.

**Primary data sources** for the implementation include authoritative geospatial datasets from the **Landesamt für Geoinformation und Landentwicklung Baden-Württemberg (LGL BW)**, which provides high-quality building geometry data in CityGML format conforming to German national spatial data infrastructure standards. The LGL BW datasets ensure geometric accuracy and semantic consistency essential for precise building identification and spatial visualization within the mixed reality environment. These datasets include detailed building footprints, height information, and structural metadata that form the foundation for 3D tileset generation.

**Energy consumption data** is sourced through the **STEG REST API** ecosystem, providing real-time building energy performance metrics with temporal granularity suitable for dynamic visualization requirements. The API infrastructure delivers energy consumption values, efficiency ratings, and associated color coding for visual representation, with authentication mechanisms ensuring data security and access control. Data quality validation includes automated range checking for energy values, coordinate system verification for geometric data, and consistency validation between building identifiers across different data sources.

**Data integration challenges** addressed in the methodology include coordinate system harmonization between LGL BW datasets (typically ETRS89/UTM Zone 32N) and the web-based visualization requirements, temporal synchronization between static building geometry and dynamic energy data, and identifier mapping between German administrative building codes and energy database references. The implementation employs robust error handling and data validation strategies to ensure visualization reliability despite potential inconsistencies in source data quality or availability.

### 4.5 Real-Time Data Modification and Synchronization

The data modification system supports real-time editing of building attributes through the user interface, with immediate synchronization to the backend API. The modification process implements optimistic update strategies, immediately reflecting changes in the client interface while asynchronously updating the backend data store.

Error handling mechanisms ensure that failed update operations are properly communicated to the user, with rollback capabilities to restore previous state when backend updates fail. The synchronization process includes conflict resolution strategies for scenarios where multiple clients modify the same building attributes simultaneously.

The update propagation mechanism ensures that successful modifications trigger immediate cache updates and visual representation refreshes, providing immediate feedback to the user while maintaining system responsiveness. The implementation includes debouncing mechanisms to prevent excessive API calls during rapid user input while maintaining real-time responsiveness for critical updates.

## 6. Mixed Reality Deployment and Cross-Platform Extension

### 6.1 HoloLens 2 Platform Adaptation Methodology

The extension of the desktop-based building energy visualization system to Microsoft HoloLens 2 represents a significant architectural consideration that leverages Unreal Engine's cross-platform capabilities while addressing the unique constraints and opportunities of mixed reality environments. The HoloLens 2 deployment methodology implements a component-based approach that maintains functional equivalence across platforms while optimizing for the specific interaction paradigms and performance characteristics of augmented reality devices.

The platform adaptation strategy employs conditional compilation and runtime platform detection to enable single-codebase deployment across desktop and HoloLens environments. The implementation utilizes Unreal Engine's platform abstraction layer to automatically configure rendering settings, input systems, and performance parameters appropriate for each target platform. For HoloLens 2 deployment, this includes automatic adjustment of rendering quality settings, frame rate targets, and memory management strategies to ensure stable 60 FPS performance within the device's computational constraints.

The mixed reality spatial context provides additional opportunities for energy visualization that are unavailable in traditional desktop environments. The system leverages HoloLens 2's spatial mapping capabilities to anchor energy data visualizations to real-world locations, enabling on-site building inspections where energy consumption data can be overlaid directly onto physical building structures. This spatial anchoring methodology implements persistent coordinate system mapping between the digital twin representation and real-world geographic coordinates.

### 6.2 Gesture-Based Interaction and Input Conversion

The adaptation to HoloLens 2's gesture-based interaction model requires fundamental reconceptualization of user interface paradigms from mouse and keyboard inputs to air tap, tap-and-hold, and gaze-based selection mechanisms. The implementation employs a custom input conversion component (`UHoloLensInputConverter`) that abstracts platform-specific input methods while maintaining consistent functionality across deployment targets.

The gesture recognition system translates traditional desktop interactions to mixed reality equivalents: air tap gestures replace left mouse clicks for building selection, tap-and-hold gestures provide context menu access equivalent to right-click functionality, and head gaze direction serves as the primary cursor mechanism for 3D object selection. The conversion system implements appropriate tolerance and timing parameters to ensure reliable gesture recognition while minimizing false positive activations.

The spatial interaction methodology leverages HoloLens 2's head tracking and hand tracking capabilities to enable natural building selection through direct gaze and gesture combinations. The ray casting system utilizes the user's head orientation to project selection rays into the 3D scene, with air tap gestures confirming selection actions. This approach provides intuitive spatial interaction that leverages the mixed reality environment's inherent advantages over traditional 2D interface paradigms.

### 6.3 Performance Optimization and Resource Management

HoloLens 2's computational constraints require specific performance optimization strategies that differ significantly from desktop deployment considerations. The methodology implements adaptive rendering quality management that automatically reduces polygon counts, texture resolutions, and lighting effects to maintain stable frame rates essential for user comfort and device thermal management.

The memory management strategy addresses HoloLens 2's limited RAM capacity through intelligent cache size limitations and aggressive garbage collection policies. The data polling frequency is automatically adjusted based on platform capabilities, reducing network activity from 30-second intervals on desktop to 60-second intervals on HoloLens to conserve battery life and processing resources.

The background processing optimization moves heavy JSON parsing operations to separate threads to prevent UI blocking, while implementing progressive data loading strategies that prioritize visible buildings over those outside the user's current field of view. This spatial culling approach significantly reduces processing overhead in large building datasets while maintaining responsive interaction for immediately relevant data.

### 6.4 User Interface Adaptation for Spatial Computing

The user interface adaptation for HoloLens 2 employment requires fundamental redesign of information presentation paradigms from traditional 2D screen-based layouts to 3D spatial interfaces suitable for mixed reality environments. The methodology implements floating interface panels that maintain consistent distance and orientation relative to the user's position, ensuring optimal readability while preventing visual obstruction of the physical environment.

The touch target optimization increases interface element sizes to accommodate finger-based interaction rather than precise mouse cursor positioning. Minimum touch target sizes are enforced to ensure reliable interaction success, with visual feedback mechanisms providing clear indication of interface element states and interaction boundaries.

The voice command integration framework provides hands-free operation capabilities essential for field deployment scenarios where manual interaction may be impractical or unsafe. The methodology includes natural language processing for common energy data queries, enabling voice commands such as "show buildings with high energy consumption" or "display energy comparison for the current building," transforming the system into a conversational interface for energy data exploration.

## Conclusion

This methodology provides a comprehensive framework for implementing real-time building energy visualization within Unreal Engine using Cesium 3D tilesets. The approach addresses key technical challenges including reliable data synchronization, robust coordinate system management, and effective visual representation strategies. The implementation demonstrates the feasibility of integrating complex geospatial datasets with modern game engine technologies to create interactive, real-time visualization applications for energy management and urban planning applications.

The modular architecture ensures maintainability and extensibility, supporting future enhancements such as additional energy metrics, alternative visualization methods, and expanded user interaction capabilities. The robust error handling and fallback mechanisms provide system reliability suitable for production deployment scenarios.