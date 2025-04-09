#!/usr/bin/env python3
import requests
import os

def populate_video_cache(video_path, proxy_url="http://localhost:8888"):
    """Populate the TrafficServer cache with a video file."""
    
    # Read the video file in chunks
    chunk_size = 65536  # 64KB chunks
    video_size = os.path.getsize(video_path)
    
    # Cache entry details
    host = "sample.example.com"  # Match the host we're using in Firefox
    path = "/video"
    
    print(f"\nPopulating cache for {host}{path}")
    print(f"Video size: {video_size} bytes")
    
    headers = {
        "Host": host,
        "Target-Path": path,
        "Content-Type": "video/mp4",
        "Content-Length": str(video_size)
    }
    
    # Open file in binary mode and stream it
    with open(video_path, 'rb') as f:
        # Send request to cache population endpoint
        response = requests.post(
            f"{proxy_url}/cache/populate",
            headers=headers,
            data=f
        )
    
    if response.status_code == 200:
        print(f"Successfully cached video at {host}{path}")
        print("\nYou can now access the video at:")
        print(f"http://{host}{path}")
    else:
        print(f"Failed to cache video: {response.status_code}")
        print(f"Response: {response.text}")

if __name__ == "__main__":
    video_path = "sample.mp4"  # Video file in the same directory
    populate_video_cache(video_path)
