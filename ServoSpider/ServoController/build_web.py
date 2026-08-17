#!/usr/bin/env python3
"""
Build script to combine web/ files into include/html.h

This script runs automatically before each build (configured in platformio.ini).
It combines index.html, style.css, and script.js into the html.h header file.
"""

Import("env")
from pathlib import Path
import re

def inline_css(html, css_content):
    """Replace <link rel="stylesheet" href="style.css"> with inline <style>"""
    style_tag = f'  <style>\n{css_content}\n  </style>'
    # Use a simple string replace instead of regex to avoid escaping issues
    html = html.replace('  <link rel="stylesheet" href="style.css">\n', style_tag + '\n')
    return html

def inline_js(html, js_content):
    """Replace <script src="script.js"></script> with inline <script>"""
    script_tag = f'  <script>\n{js_content}\n  </script>'
    # Use a simple string replace instead of regex to avoid escaping issues
    html = html.replace('  <script src="script.js"></script>\n', script_tag + '\n')
    return html

def build_html_header():
    """Combine web files into html.h"""
    project_dir = Path(env['PROJECT_DIR'])
    web_dir = project_dir / 'web'
    include_dir = project_dir / 'include'

    html_path = web_dir / 'index.html'
    css_path = web_dir / 'style.css'
    js_path = web_dir / 'script.js'
    output_path = include_dir / 'html.h'

    # Check if web files exist
    if not html_path.exists():
        print(f"Warning: {html_path} not found, skipping web build")
        return

    print("Building html.h from web/ sources...")

    # Read source files
    html = html_path.read_text(encoding='utf-8')
    css = css_path.read_text(encoding='utf-8') if css_path.exists() else ""
    js = js_path.read_text(encoding='utf-8') if js_path.exists() else ""

    # Inline CSS and JS
    if css:
        html = inline_css(html, css)
    if js:
        html = inline_js(html, js)

    # Create header file content
    header_content = f"""// HTML page for configuration
const char* htmlPage = R"rawliteral({html})rawliteral";
"""

    # Write output
    output_path.write_text(header_content, encoding='utf-8')
    print(f"[OK] Generated {output_path}")

# Run the build
try:
    build_html_header()
except Exception as e:
    print(f"Error building html.h: {e}")
    import traceback
    traceback.print_exc()
