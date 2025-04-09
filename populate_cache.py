#!/usr/bin/env python3
import requests
import json

PROXY_URL = 'http://localhost:8888'

def populate_cache_entry(host, path, content_type, content):
    url = f'{PROXY_URL}/cache/populate'
    headers = {
        'Host': host,
        'Target-Path': path,
        'Content-Type': content_type,
        'Content-Length': str(len(content))
    }
    response = requests.post(url, headers=headers, data=content)
    print(f'Populating cache for {host}{path}:')
    print(f'Status: {response.status_code}')
    print(f'Response: {response.text}\n')

# Example entries to cache
entries = [
    {
        'host': 'example.com',
        'path': '/about',
        'content_type': 'text/html',
        'content': '''<!DOCTYPE html>
<html>
<head>
    <title>About Example</title>
</head>
<body>
    <h1>About Example.com</h1>
    <p>This is a pre-populated cache entry for the about page.</p>
</body>
</html>'''
    },
    {
        'host': 'example.com',
        'path': '/api/data',
        'content_type': 'application/json',
        'content': json.dumps({
            'message': 'This is cached API data',
            'timestamp': '2025-04-08T16:00:00Z',
            'data': {
                'id': 123,
                'name': 'Test Entry',
                'value': 42
            }
        })
    },
    {
        'host': 'test.example.com',
        'path': '/',
        'content_type': 'text/html',
        'content': '''<!DOCTYPE html>
<html>
<head>
    <title>Test Domain</title>
</head>
<body>
    <h1>Test Example Domain</h1>
    <p>This is a pre-populated cache entry for test.example.com.</p>
</body>
</html>'''
    }
]

def main():
    print('Starting cache population...\n')
    for entry in entries:
        populate_cache_entry(
            entry['host'],
            entry['path'],
            entry['content_type'],
            entry['content']
        )
    print('Cache population complete!')

if __name__ == '__main__':
    main()
