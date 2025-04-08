#!/usr/bin/env python3
import requests
import json

def populate_cache(proxy_url="http://localhost:8888"):
    """Populate the TrafficServer cache with test content."""
    test_entries = [
        {
            "host": "example.com",
            "path": "/about",
            "content": """<!DOCTYPE html>
<html>
<head>
    <title>About Example.com</title>
</head>
<body>
    <h1>About Example.com</h1>
    <p>This is a pre-populated cache entry for example.com/about.</p>
</body>
</html>""",
            "content_type": "text/html"
        },
        {
            "host": "example.com",
            "path": "/api/data",
            "content": json.dumps({
                "message": "Test data from cache",
                "timestamp": "2025-04-08T12:00:00Z"
            }),
            "content_type": "application/json"
        },
        {
            "host": "test.example.com",
            "path": "/",
            "content": """<!DOCTYPE html>
<html>
<head>
    <title>Test Domain</title>
</head>
<body>
    <h1>Test Example Domain</h1>
    <p>This is a pre-populated cache entry for test.example.com.</p>
</body>
</html>""",
            "content_type": "text/html"
        }
    ]

    for entry in test_entries:
        print(f"\nPopulating cache for {entry['host']}{entry['path']}")
        headers = {
            "Host": entry["host"],
            "Target-Path": entry["path"],
            "Content-Type": entry["content_type"]
        }
        response = requests.post(
            f"{proxy_url}/cache/populate",
            headers=headers,
            data=entry["content"]
        )
        if response.status_code == 200:
            print(f"Successfully cached {entry['host']}{entry['path']}")
        else:
            print(f"Failed to cache {entry['host']}{entry['path']}: {response.status_code}")

if __name__ == "__main__":
    populate_cache()
    print("\nCache population complete!")
