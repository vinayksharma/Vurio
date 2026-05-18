import json

def convert():
    with open('dedup_server/config/config.descriptions.json', 'r') as f:
        data = json.load(f)
    
    match_score_ast = [
        { "type": "headline", "text": "This dual slider controls two key properties that define how images are grouped:" },
        { "type": "paragraph", "text": "**Similar Threshold (Green/Down)**: Sets the maximum allowed visual distance to cluster images into a group. A higher value casts a wider net, grouping looser similarities together." },
        { "type": "paragraph", "text": "**Exact Threshold (Red/Up)**: Sets the strictness for identifying identical matches. A lower value requires a near-perfect match to be considered an exact duplicate." },
        { "type": "paragraph", "text": "**In Combination**: The Similar threshold defines the boundaries of the entire group cluster, while the Exact threshold highlights the truly identical pairs within that specific group for operations like auto-eject." }
    ]

    for key, value in data.items():
        if key.endswith('.long'):
            if key in ["duplicates.threshold.similarity.long", "duplicates.threshold.duplicate.long"]:
                data[key] = match_score_ast
            else:
                if isinstance(value, str):
                    data[key] = [{ "type": "paragraph", "text": value }]
    
    with open('dedup_server/config/config.descriptions.json', 'w') as f:
        json.dump(data, f, indent=4)

if __name__ == "__main__":
    convert()
