#pragma once

#define IDI_APP             101
#define IDD_SETTINGS        201
#define IDD_GOTO            202

#define IDC_APIKEY          1001
#define IDC_SHOWKEY         1002
#define IDC_MODEL           1003
#define IDC_REFRESH         1004
#define IDC_VERIFY          1005
#define IDC_EFFORT          1006
#define IDC_CONTEXT         1007
#define IDC_CONTEXT_LBL     1008
#define IDC_STATUS          1009
#define IDC_KEYHINT         1010
#define IDC_LINE            1011
#define IDC_LBL1            1012
#define IDC_LBL2            1013
#define IDC_LBL3            1014
#define IDC_LBL4            1015

// ---- menu / command ids
#define CMD_NEW             40001
#define CMD_NEWWINDOW       40002
#define CMD_OPEN            40003
#define CMD_SAVE            40004
#define CMD_SAVEAS          40005
#define CMD_SAVEALL         40006
#define CMD_RELOAD          40007
#define CMD_CLOSETAB        40008
#define CMD_EXIT            40009
#define CMD_CLEARRECENT     40010
#define CMD_OPENFOLDER      40011

#define CMD_UNDO            40101
#define CMD_REDO            40102
#define CMD_CUT             40103
#define CMD_COPY            40104
#define CMD_PASTE           40105
#define CMD_SELECTALL       40106
#define CMD_FIND            40107
#define CMD_FINDNEXT        40108
#define CMD_FINDPREV        40109
#define CMD_REPLACE         40110
#define CMD_GOTO            40111
#define CMD_BOLD            40112
#define CMD_ITALIC          40113
#define CMD_CODE            40114
#define CMD_LINK            40115
#define CMD_DELETE          40116
#define CMD_PASTEIMAGE      40117

#define CMD_VIEW_EDITOR     40201
#define CMD_VIEW_SPLIT      40202
#define CMD_VIEW_PREVIEW    40203
#define CMD_VIEW_TOGGLE     40204
#define CMD_WRAP            40205
#define CMD_LINENUMBERS     40206
#define CMD_LIMITWIDTH      40207
#define CMD_ZOOMIN          40208
#define CMD_ZOOMOUT         40209
#define CMD_ZOOMRESET       40210
#define CMD_THEME_SYSTEM    40211
#define CMD_THEME_LIGHT     40212
#define CMD_THEME_DARK      40213
#define CMD_FONT            40214
#define CMD_NEXTTAB         40215
#define CMD_PREVTAB         40216
#define CMD_RESTORESESSION  40217

#define CMD_SPELL_TOGGLE    40301
#define CMD_SPELL_CAPS      40302
#define CMD_SPELL_WINLANG   40303
#define CMD_SPELL_LANG0     40400   // + index (up to 199)
#define CMD_SPELL_SUGG0     40600   // + index
#define CMD_SPELL_ADD0      40650   // + language index
#define CMD_SPELL_IGNORE    40699

#define CMD_AI_REWRITE      40701
#define CMD_AI_SETTINGS     40702

#define CMD_EOL_CRLF        40801
#define CMD_EOL_LF          40802
#define CMD_ENC_UTF8        40811
#define CMD_ENC_UTF8BOM     40812
#define CMD_ENC_UTF16LE     40813

#define CMD_ABOUT           40901
#define CMD_MDHELP          40902

#define CMD_RECENT0         41000   // + index (up to 20)

#define CMD_TOOLBAR         40218

// Format (Markdown) commands
#define CMD_FMT_HEADING     41100   // toolbar dropdown: paragraph style
#define CMD_FMT_PARA        41101   // normal text
#define CMD_FMT_H1          41102   // CMD_FMT_H1 + n - 1 for heading n (1..6)
#define CMD_FMT_STRIKE      41110
#define CMD_FMT_UNDERLINE   41111
#define CMD_FMT_KBD         41112
#define CMD_FMT_MATH        41113
#define CMD_FMT_CLEAR       41114
#define CMD_FMT_BULLET      41120
#define CMD_FMT_NUMBER      41121
#define CMD_FMT_TASK        41122
#define CMD_FMT_TASKDONE    41123
#define CMD_FMT_INDENT      41124
#define CMD_FMT_OUTDENT     41125
#define CMD_FMT_QUOTE       41126
#define CMD_FMT_CALLOUT     41130   // toolbar dropdown
#define CMD_FMT_CALLOUT0    41131   // + 0..4: note, tip, important, warning, caution
#define CMD_FMT_CODEBLOCK   41140
#define CMD_FMT_MATHBLOCK   41141
#define CMD_FMT_IMAGE       41150
#define CMD_FMT_IMAGEURL    41151
#define CMD_FMT_TABLE       41160   // toolbar dropdown
#define CMD_FMT_TABLE0      41161   // + size index (see Fmt_TableSizes)
#define CMD_FMT_ROWABOVE    41170
#define CMD_FMT_ROWBELOW    41171
#define CMD_FMT_COLLEFT     41172
#define CMD_FMT_COLRIGHT    41173
#define CMD_FMT_DELROW      41174
#define CMD_FMT_DELCOL      41175
#define CMD_FMT_ALIGNLEFT   41176
#define CMD_FMT_ALIGNCENTER 41177
#define CMD_FMT_ALIGNRIGHT  41178
#define CMD_FMT_ALIGNNONE   41179
#define CMD_FMT_TABLETIDY   41180
#define CMD_FMT_HR          41185
#define CMD_FMT_FOOTNOTE    41186
#define CMD_FMT_LINEBREAK   41187
#define CMD_FMT_MORE        41190   // toolbar dropdown: everything else
#define CMD_TB_OVERFLOW     41199   // toolbar: hidden buttons
