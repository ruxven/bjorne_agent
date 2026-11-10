/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "bjorne_agent", "index.html", [
    [ "bjorne_agent", "md_README.html", [
      [ "Contents", "md_README.html#autotoc_md1", null ],
      [ "Requirements", "md_README.html#autotoc_md2", null ],
      [ "Build", "md_README.html#autotoc_md3", null ],
      [ "Install", "md_README.html#autotoc_md4", [
        [ "Package", "md_README.html#autotoc_md5", null ],
        [ "Cross build for Windows", "md_README.html#autotoc_md6", null ],
        [ "Cross build for ARM (Raspberry Pi)", "md_README.html#autotoc_md7", null ]
      ] ],
      [ "Usage", "md_README.html#autotoc_md8", [
        [ "Output levels", "md_README.html#autotoc_md9", null ],
        [ "Exit codes", "md_README.html#autotoc_md10", null ],
        [ "Setting up a config", "md_README.html#autotoc_md11", null ]
      ] ],
      [ "Configuration", "md_README.html#autotoc_md12", [
        [ "Agent", "md_README.html#autotoc_md13", null ],
        [ "Model", "md_README.html#autotoc_md14", null ],
        [ "TLS", "md_README.html#autotoc_md15", null ]
      ] ],
      [ "Tools", "md_README.html#autotoc_md16", null ],
      [ "Containers", "md_README.html#autotoc_md17", null ],
      [ "Development", "md_README.html#autotoc_md18", null ],
      [ "License", "md_README.html#autotoc_md19", null ]
    ] ],
    [ "SPEC: C++ Agent Harness", "md_SPEC.html", [
      [ "Purpose", "md_SPEC.html#autotoc_md21", null ],
      [ "Goals", "md_SPEC.html#autotoc_md22", null ],
      [ "Version One Scope", "md_SPEC.html#autotoc_md23", null ],
      [ "Language and Toolchain", "md_SPEC.html#autotoc_md24", null ],
      [ "Dependencies", "md_SPEC.html#autotoc_md25", null ],
      [ "Configuration", "md_SPEC.html#autotoc_md26", [
        [ "Format", "md_SPEC.html#autotoc_md27", null ],
        [ "Location", "md_SPEC.html#autotoc_md28", null ],
        [ "Top level", "md_SPEC.html#autotoc_md29", null ],
        [ "Agent", "md_SPEC.html#autotoc_md30", null ],
        [ "Model", "md_SPEC.html#autotoc_md31", null ]
      ] ],
      [ "Context Management", "md_SPEC.html#autotoc_md32", null ],
      [ "Build and TLS", "md_SPEC.html#autotoc_md33", null ],
      [ "Container", "md_SPEC.html#autotoc_md34", null ],
      [ "Next Steps", "md_SPEC.html#autotoc_md35", null ],
      [ "Open Questions", "md_SPEC.html#autotoc_md36", null ]
    ] ],
    [ "SPEC.CONFIG: C++ Agent Harness Configuration Specification", "md_SPEC_8CONFIG.html", [
      [ "Purpose", "md_SPEC_8CONFIG.html#autotoc_md38", null ],
      [ "Scope", "md_SPEC_8CONFIG.html#autotoc_md39", null ],
      [ "Overview", "md_SPEC_8CONFIG.html#autotoc_md40", null ],
      [ "Config File Location", "md_SPEC_8CONFIG.html#autotoc_md41", null ],
      [ "Top Level", "md_SPEC_8CONFIG.html#autotoc_md42", null ],
      [ "Agent", "md_SPEC_8CONFIG.html#autotoc_md43", null ],
      [ "Models", "md_SPEC_8CONFIG.html#autotoc_md44", null ],
      [ "TLS", "md_SPEC_8CONFIG.html#autotoc_md45", null ],
      [ "Context Management", "md_SPEC_8CONFIG.html#autotoc_md46", null ],
      [ "Decisions", "md_SPEC_8CONFIG.html#autotoc_md47", null ],
      [ "Open Questions", "md_SPEC_8CONFIG.html#autotoc_md48", null ],
      [ "Status", "md_SPEC_8CONFIG.html#autotoc_md49", null ]
    ] ],
    [ "SPEC.LOOP: Agent Loop Specification", "md_SPEC_8LOOP.html", [
      [ "Purpose", "md_SPEC_8LOOP.html#autotoc_md51", null ],
      [ "Overview", "md_SPEC_8LOOP.html#autotoc_md52", null ],
      [ "Loop Stages", "md_SPEC_8LOOP.html#autotoc_md53", [
        [ "Context Assembly", "md_SPEC_8LOOP.html#autotoc_md54", null ],
        [ "Model Call", "md_SPEC_8LOOP.html#autotoc_md55", null ],
        [ "Tool Execution", "md_SPEC_8LOOP.html#autotoc_md56", null ],
        [ "Iteration", "md_SPEC_8LOOP.html#autotoc_md57", null ]
      ] ],
      [ "Stopping Conditions", "md_SPEC_8LOOP.html#autotoc_md58", null ],
      [ "Context Management", "md_SPEC_8LOOP.html#autotoc_md59", null ],
      [ "Error Handling", "md_SPEC_8LOOP.html#autotoc_md60", null ],
      [ "Status", "md_SPEC_8LOOP.html#autotoc_md61", null ]
    ] ],
    [ "Design: C++ Agent Harness", "md_DESIGN.html", [
      [ "Purpose", "md_DESIGN.html#autotoc_md63", null ],
      [ "Design Principles", "md_DESIGN.html#autotoc_md64", null ],
      [ "Architecture", "md_DESIGN.html#autotoc_md65", [
        [ "Entry Point", "md_DESIGN.html#autotoc_md66", null ],
        [ "Configuration", "md_DESIGN.html#autotoc_md67", null ],
        [ "Model Client", "md_DESIGN.html#autotoc_md68", null ],
        [ "Context Manager", "md_DESIGN.html#autotoc_md69", null ],
        [ "Agent Loop", "md_DESIGN.html#autotoc_md70", null ],
        [ "Tool Layer", "md_DESIGN.html#autotoc_md71", null ]
      ] ],
      [ "Fulfilment Mapping", "md_DESIGN.html#autotoc_md72", null ],
      [ "Build and Container Design", "md_DESIGN.html#autotoc_md73", [
        [ "Build Environments", "md_DESIGN.html#autotoc_md74", null ],
        [ "Runtime Image", "md_DESIGN.html#autotoc_md75", null ]
      ] ],
      [ "Design Decisions", "md_DESIGN.html#autotoc_md76", null ],
      [ "Status", "md_DESIGN.html#autotoc_md77", null ]
    ] ],
    [ "ROADMAP: C++ Agent Harness", "md_ROADMAP.html", [
      [ "Phase 1: Specification Development", "md_ROADMAP.html#autotoc_md79", null ],
      [ "Phase 2: Design", "md_ROADMAP.html#autotoc_md80", null ],
      [ "Phase 3: Implementation", "md_ROADMAP.html#autotoc_md81", null ],
      [ "Phase 4: Evolution", "md_ROADMAP.html#autotoc_md82", [
        [ "Phase A: Foundation", "md_ROADMAP.html#autotoc_md83", null ],
        [ "Phase B: Context fragments", "md_ROADMAP.html#autotoc_md84", null ],
        [ "Phase C: Session persistence", "md_ROADMAP.html#autotoc_md85", null ],
        [ "Phase D: Interaction", "md_ROADMAP.html#autotoc_md86", null ],
        [ "Phase E: MCP", "md_ROADMAP.html#autotoc_md87", null ],
        [ "Phase F: ACP client mode", "md_ROADMAP.html#autotoc_md88", null ],
        [ "Shared JSON-RPC core", "md_ROADMAP.html#autotoc_md89", null ],
        [ "Tool set customization", "md_ROADMAP.html#autotoc_md90", null ],
        [ "Deliberately out of scope", "md_ROADMAP.html#autotoc_md91", null ],
        [ "Spec impact", "md_ROADMAP.html#autotoc_md92", null ]
      ] ],
      [ "Quality Gates", "md_ROADMAP.html#autotoc_md93", null ]
    ] ],
    [ "Namespaces", "namespaces.html", [
      [ "Namespace List", "namespaces.html", "namespaces_dup" ],
      [ "Namespace Members", "namespacemembers.html", [
        [ "All", "namespacemembers.html", null ],
        [ "Functions", "namespacemembers_func.html", null ],
        [ "Variables", "namespacemembers_vars.html", null ],
        [ "Typedefs", "namespacemembers_type.html", null ],
        [ "Enumerations", "namespacemembers_enum.html", null ]
      ] ]
    ] ],
    [ "Classes", "annotated.html", [
      [ "Class List", "annotated.html", "annotated_dup" ],
      [ "Class Index", "classes.html", null ],
      [ "Class Hierarchy", "hierarchy.html", "hierarchy" ],
      [ "Class Members", "functions.html", [
        [ "All", "functions.html", null ],
        [ "Functions", "functions_func.html", null ],
        [ "Variables", "functions_vars.html", null ],
        [ "Enumerations", "functions_enum.html", null ]
      ] ]
    ] ],
    [ "Files", "files.html", [
      [ "File List", "files.html", "files_dup" ],
      [ "File Members", "globals.html", [
        [ "All", "globals.html", null ],
        [ "Functions", "globals_func.html", null ]
      ] ]
    ] ]
  ] ]
];

var NAVTREEINDEX =
[
"AgentLoop_8cpp.html",
"main_8cpp.html#a3c04138a5bfe5d72780bb7e82a18e627",
"structbjorne__agent_1_1http_1_1OpenSslTransport_1_1Options.html#ab99629283a6a982082fb3cbd07f50a02"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';