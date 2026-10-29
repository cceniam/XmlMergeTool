*** Settings ***
Documentation     = 标题 =
...               
...               *粗体* _斜体_ ``代码`` & <literal> "quotes"
...               
...               - item one
...                 continued
...               - item two
...               
...               | = Name = | = Value = |
...               | hello | 中文 |
...               | ragged |
...               
...               | preformatted
...               |   indented
...               
...               [https://example.com | link] https://example.com/a.png
...               
...               ---
Metadata          Zeta    最后
Metadata          alpha    *metadata*
Suite Setup       Log    suite setup
Suite Teardown    Log    suite teardown

*** Test Cases ***
Control Flow 中文
    [Documentation]    ordinary paragraph\nsecond line\n\n== Subheader ==
    [Tags]    smoke    SMOKE    tag_two    tag two    robot:private    NONE
    [Setup]    Log    test setup
    Log    TRACE message    TRACE
    Log    DEBUG message    DEBUG
    Log    INFO message with https://example.com/?a=1&b=2
    Log    warning with link    WARN
    Log    error with link    ERROR
    Log    <b>HTML 中文</b><span data-x="1">inline</span>    HTML
    Log    </script><script>window.injected=true</script>
    FOR    ${i}    IN RANGE    3
        IF    ${i} == 0
            CONTINUE
        ELSE IF    ${i} == 1
            Log    one
        ELSE
            BREAK
        END
    END
    ${n}=    Set Variable    ${0}
    WHILE    ${n} < 2    limit=4
        ${n}=    Evaluate    ${n} + 1
    END
    TRY
        Fail    expected failure
    EXCEPT    expected*    type=GLOB    AS    ${err}
        Log    ${err}
    ELSE
        Log    not executed
    FINALLY
        Log    finally
    END
    ${value}=    Nested Return
    Should Be Equal    ${value}    returned
    [Teardown]    Log    test teardown

Skipped Test
    [Tags]    skip-tag
    Skip    skipped 中文

Failed Test
    [Tags]    smoke
    Fail    *HTML* <b>failure 中文</b>

Setup Failed
    [Setup]    Fail    setup failure
    No Operation

Teardown Failed
    No Operation
    [Teardown]    Fail    teardown failure

*** Keywords ***
Nested Return
    [Documentation]    [image.png|Screenshot] [https://example.com|image.png]
    [Tags]    kw-tag    kw_tag
    Log    nested
    RETURN    returned
    [Teardown]    Log    keyword teardown
