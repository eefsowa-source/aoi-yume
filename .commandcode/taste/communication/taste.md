# Communication preferences

- Prefers responses in Korean; frequently writes messages in Korean and explicitly asked "korean plz" and "한글로말해봐". The preference extends to generated artifacts too — after the agent created an English agent definition file, the user said "한글로" and expected the file's description + system prompt rewritten in Korean. Confidence: 0.97
- Wants the agent to ask which specific tasks to handle (e.g., which pull requests/issues to fix) before starting work. Confidence: 0.6
- Communicates in very terse, short commands ("1", "해", "hit", "yo", "what u recommend", "추천", "2-3-1-4", "더 해야할건?", and single-word answers like "후자" to binary questions) and expects the agent to infer intent from minimal input. Confidence: 0.96
- Expects answers about tool/product capabilities to be grounded in official documentation/skills rather than assumed knowledge (user asked "codex에서 subagent로 command code 쓸수있어?"; the agent activated the command-code-knowledge skill and read the official headless/custom-agents references before answering). Confidence: 0.6
- Uses the terse command "cmd--yolo" to instruct the agent to proceed without further confirmation — i.e., skip safety-style check-ins and just run the next command (build, test, etc.) straight through. Confidence: 0.8
