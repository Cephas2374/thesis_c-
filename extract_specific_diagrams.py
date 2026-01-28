import fitz  # PyMuPDF
import os
from pathlib import Path
import cv2
import numpy as np

def extract_specific_figures():
    """
    Target specific papers that are most likely to contain the desired diagrams
    """
    base_dir = Path("d:/ar_thesis_cephas/unreal/Final_project/latex")
    output_dir = base_dir / "png"
    
    # Target specific papers based on relevance to topics
    target_papers = [
        # CityGML and 3D Modeling
        {
            "path": base_dir / "CityGML-based schema design in spatial databases_20260113_114822/pdfs/A Prototype Architecture for Interactive 3D Maps on the Web.pdf",
            "category": "3D Web Architecture",
            "expected": ["web_3d_maps_prototype_architecture", "webgl_rendering_pipeline"]
        },
        # Cesium and 3D Tiles
        {
            "path": base_dir / "CesiumJS Rendering pipelines_20260113_031325/pdfs/arxiv_2001.11245v1.pdf",
            "category": "Cesium Rendering",
            "expected": ["cesium_3dtiles_loading_pipeline", "level_of_detail_hierarchy"]
        },
        {
            "path": base_dir / "CesiumJS Rendering pipelines_20260113_031325/pdfs/arxiv_2005.01637v2.pdf",
            "category": "Cesium Rendering",
            "expected": ["cesium_viewer_architecture", "3dtiles_streaming_workflow"]
        },
        # REST APIs
        {
            "path": base_dir / "RESTful and real-time APIs in geospatial platforms_20260113_031515/pdfs/arxiv_1806.01814v4.pdf",
            "category": "RESTful APIs",
            "expected": ["rest_api_endpoint_design", "geospatial_service_architecture"]
        },
        {
            "path": base_dir / "RESTful and real-time APIs in geospatial platforms_20260113_031515/pdfs/arxiv_1810.11966v3.pdf",
            "category": "RESTful APIs", 
            "expected": ["microservices_communication_pattern", "api_gateway_architecture"]
        }
    ]
    
    extracted = []
    
    for paper_info in target_papers:
        if paper_info["path"].exists():
            print(f"\nProcessing: {paper_info['path'].name}")
            images = extract_large_diagrams(paper_info["path"], output_dir, paper_info["category"], paper_info["expected"])
            extracted.extend(images)
    
    return extracted

def extract_large_diagrams(pdf_path, output_dir, category, expected_names):
    """
    Extract larger, more complex diagrams that are likely to be architectural
    """
    extracted_images = []
    
    try:
        pdf_document = fitz.open(pdf_path)
        pdf_name = Path(pdf_path).stem
        
        image_count = 0
        
        # Focus on pages 2-12 where diagrams are most common
        for page_num in range(1, min(len(pdf_document), 13)):
            page = pdf_document[page_num]
            image_list = page.get_images()
            
            for img_index, img in enumerate(image_list):
                if image_count >= 3:  # Limit per paper
                    break
                
                # Extract image
                xref = img[0]
                pix = fitz.Pixmap(pdf_document, xref)
                
                # Focus on larger images (likely to be diagrams)
                if pix.width < 400 or pix.height < 250:
                    pix = None
                    continue
                
                if pix.n - pix.alpha < 4:  # GRAY or RGB
                    temp_path = os.path.join(output_dir, f"temp_{page_num}_{img_index}.png")
                    pix.save(temp_path)
                    
                    # Enhanced relevance check for architectural diagrams
                    if is_architectural_diagram(temp_path):
                        # Use expected names or generate descriptive names
                        if image_count < len(expected_names):
                            img_name = f"{expected_names[image_count]}.png"
                        else:
                            img_name = f"{category.lower().replace(' ', '_')}_diagram_{image_count + 1}.png"
                        
                        final_path = os.path.join(output_dir, img_name)
                        
                        # Handle naming conflicts
                        counter = 1
                        while os.path.exists(final_path):
                            name_parts = img_name.split('.')
                            img_name = f"{name_parts[0]}_{counter}.{name_parts[1]}"
                            final_path = os.path.join(output_dir, img_name)
                            counter += 1
                        
                        os.rename(temp_path, final_path)
                        
                        extracted_images.append({
                            'filename': img_name,
                            'source_paper': pdf_name,
                            'page': page_num + 1,
                            'category': category,
                            'path': final_path,
                            'dimensions': f"{pix.width}x{pix.height}"
                        })
                        
                        print(f"  ✓ {img_name} - {pix.width}x{pix.height}")
                        image_count += 1
                    else:
                        os.remove(temp_path)
                
                pix = None
        
        pdf_document.close()
        
    except Exception as e:
        print(f"Error: {str(e)}")
    
    return extracted_images

def is_architectural_diagram(img_path):
    """
    Specifically detect architectural and system diagrams
    """
    try:
        img = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)
        if img is None:
            return False
        
        height, width = img.shape
        
        # Must be reasonably large
        if width < 400 or height < 250:
            return False
        
        # Check for architectural characteristics
        
        # 1. Edge detection for structured content
        edges = cv2.Canny(img, 30, 100)
        edge_density = np.sum(edges > 0) / (width * height)
        
        # 2. Look for rectangular structures (common in architecture diagrams)
        contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        rect_count = 0
        large_rect_count = 0
        
        for contour in contours:
            area = cv2.contourArea(contour)
            if area > 50:
                # Check if roughly rectangular
                x, y, w, h = cv2.boundingRect(contour)
                aspect_ratio = w / h if h > 0 else 0
                
                if 0.3 < aspect_ratio < 3.0 and area > 200:  # Reasonable rectangles
                    rect_count += 1
                    if area > 1000:  # Large components
                        large_rect_count += 1
        
        # 3. Check for horizontal and vertical lines (common in diagrams)
        horizontal_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (25, 1))
        vertical_kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (1, 25))
        
        horizontal_lines = cv2.morphologyEx(edges, cv2.MORPH_OPEN, horizontal_kernel, iterations=1)
        vertical_lines = cv2.morphologyEx(edges, cv2.MORPH_OPEN, vertical_kernel, iterations=1)
        
        h_line_density = np.sum(horizontal_lines > 0) / (width * height)
        v_line_density = np.sum(vertical_lines > 0) / (width * height)
        
        # 4. Color analysis - diagrams often have limited, distinct colors
        img_color = cv2.imread(img_path)
        if img_color is not None:
            unique_colors = len(np.unique(img_color.reshape(-1, img_color.shape[2]), axis=0))
        else:
            unique_colors = 1000  # Fallback
        
        # 5. Contrast analysis
        std_dev = np.std(img)
        mean_val = np.mean(img)
        
        # Scoring system for architectural diagrams
        score = 0
        
        # Good edge structure
        if 0.02 < edge_density < 0.20:
            score += 3
        
        # Multiple rectangular components
        if rect_count > 3:
            score += 3
        if large_rect_count > 1:
            score += 2
        
        # Structured line content
        if h_line_density > 0.001 or v_line_density > 0.001:
            score += 2
        
        # Limited color palette (typical of diagrams)
        if unique_colors < 800:
            score += 2
        
        # Good contrast
        if std_dev > 30 and 50 < mean_val < 200:
            score += 2
        
        # Size bonus for larger images (more likely to be important diagrams)
        if width > 600 and height > 400:
            score += 1
        
        return score >= 8  # Higher threshold for architectural diagrams
        
    except Exception:
        return False

if __name__ == "__main__":
    print("Extracting specific architectural diagrams...")
    results = extract_specific_figures()
    
    print(f"\n=== Summary ===")
    print(f"Extracted {len(results)} additional diagrams:")
    for img in results:
        print(f"✓ {img['filename']} ({img['dimensions']}) - {img['source_paper']}")