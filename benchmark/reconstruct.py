"""Feeds a binary's full decompiled pseudocode (all functions) to an LLM and
asks it to (a) explain what the program as a whole does and (b) rewrite it
as a single clean, compilable, behaviorally equivalent C program.

Whole-program reconstruction (rather than function-at-a-time) is used so the
result can be compiled and run against the same CLI inputs as the ground
truth binary for the "behavioral" score (see score.py). The "descriptive"
score separately checks whether the explanation names the program's actual
purpose.

Uses the Anthropic API by default (ANTHROPIC_API_KEY env var). The model is
configurable via --model so the harness can later compare different models.
"""
import json
import re

import anthropic

RECONSTRUCTION_PROMPT = """\
You are shown decompiler-generated pseudocode for every function in a \
small compiled C program. You have no access to the original source, \
comments, or meaningful symbol names beyond what the decompiler produced \
(the real function names may also be missing or renamed by the \
decompiler -- infer from behavior, not names).

Decompiled functions (name -> pseudocode):
```
{pseudocode_json}
```

Based only on this:

1. Explain in 3-5 sentences what this program actually does as a whole \
(its real purpose/algorithm, and what a correct input looks like).
2. Rewrite it as a single, clean, compilable, standalone C program that is \
behaviorally equivalent: given the same command-line arguments, it must \
produce the same stdout and exit code as the original. Include a `main` \
function that reads `argv` the same way the original program would \
(infer the calling convention from the pseudocode). Include any \
`#include`s it needs.

Respond in exactly this format:

EXPLANATION:
<your explanation>

CODE:
```c
<your rewritten program, including main>
```
"""


def reconstruct_program(
    client: anthropic.Anthropic, model: str, decompiled_functions: dict
):
    """Returns (explanation: str, rewritten_c_code: str | None)."""
    pseudocode_json = json.dumps(decompiled_functions, indent=2)
    message = client.messages.create(
        model=model,
        max_tokens=4096,
        messages=[
            {
                "role": "user",
                "content": RECONSTRUCTION_PROMPT.format(
                    pseudocode_json=pseudocode_json
                ),
            }
        ],
    )
    text = "".join(block.text for block in message.content if block.type == "text")

    explanation = ""
    exp_match = re.search(r"EXPLANATION:\s*(.*?)\s*CODE:", text, re.DOTALL)
    if exp_match:
        explanation = exp_match.group(1).strip()

    code = None
    code_match = re.search(r"```c\s*(.*?)```", text, re.DOTALL)
    if code_match:
        code = code_match.group(1).strip()

    return explanation, code


def judge_explanation(
    client: anthropic.Anthropic, model: str, ground_truth_doc: str, explanation: str
) -> dict:
    """Rubric-scores `explanation` against a human-written ground-truth
    description of the original program. Used for the "descriptive"
    benchmark metric.
    """
    prompt = f"""\
You are scoring how well an AI-generated explanation of a decompiled \
program matches the program's real, ground-truth purpose.

Ground truth (what the program actually does):
{ground_truth_doc}

AI-generated explanation (from decompiler pseudocode alone):
{explanation}

Score the AI explanation from 0 to 5:
5 = correctly identifies the specific algorithm/purpose
3 = identifies the general category of behavior but misses specifics
1 = vague or generic, doesn't meaningfully explain the program
0 = wrong or no explanation

Respond as JSON only: {{"score": <int 0-5>, "justification": "<one sentence>"}}
"""
    message = client.messages.create(
        model=model,
        max_tokens=256,
        messages=[{"role": "user", "content": prompt}],
    )
    text = "".join(block.text for block in message.content if block.type == "text")
    json_match = re.search(r"\{.*\}", text, re.DOTALL)
    if not json_match:
        return {"score": 0, "justification": "judge did not return parseable JSON"}
    return json.loads(json_match.group(0))
