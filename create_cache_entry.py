#!/usr/bin/env python3
import time
import os
import struct
import sys

def create_cache_entry(url, content, content_type="text/html", etag=""):
    # Create hash of URL as filename (matching C++ std::hash behavior)
    # Note: This is a simple hash implementation, might need adjustment to match C++ std::hash
    filename = str(hash(url))
    
    # Current timestamp
    timestamp = int(time.time())
    
    print(f"Creating cache entry:")
    print(f"URL: {url}")
    print(f"Hash/Filename: {filename}")
    print(f"Content length: {len(content)}")
    
    # Create binary file
    with open(filename, 'wb') as f:
        # Write timestamp (8 bytes)
        f.write(struct.pack('q', timestamp))
        
        # Write content-type length and string
        f.write(struct.pack('q', len(content_type)))
        f.write(content_type.encode())
        
        # Write etag length and string
        f.write(struct.pack('q', len(etag)))
        f.write(etag.encode())
        
        # Write content length and string
        f.write(struct.pack('q', len(content)))
        f.write(content.encode())
    
    print(f"\nCache file created: {filename}")
    return filename

if __name__ == "__main__":
    # Example content
    test_content = """
    <html>
    <head><title>Test Cached Page</title></head>
    <body>
        <h1>This is a cached test page</h1>
        <p>This content was pre-cached in the TrafficServer proxy.</p>
        <p>Current timestamp: {}</p>
    </body>
    </html>
    """.format(time.strftime("%Y-%m-%d %H:%M:%S"))
    
    # Create cache entry for http://example.com/test
    filename = create_cache_entry(
        "http://example.com/test",
        test_content,
        "text/html",
        "w/" + str(int(time.time()))  # Generate a simple ETag
    )
