*** Settings ***
Documentation    Advanced report transformations.
Suite Setup      Fixture With Teardown
Suite Teardown   Fixture With Teardown

*** Test Cases ***
Passing With Fixtures
    [Setup]    Fixture With Teardown
    FOR    ${i}    IN RANGE    4
        IF    ${i} < 2
            Nested Keyword
        ELSE
            Log    remaining ${i}
        END
    END
    ${n}=    Set Variable    ${0}
    WHILE    ${n} < 4
        ${n}=    Evaluate    ${n} + 1
        Nested Keyword
    END
    TRY
        Fail    caught failure
    EXCEPT    caught failure
        Nested Keyword
    FINALLY
        Log    finally retained
    END
    [Teardown]    Fixture With Teardown

Warning And Error In Loops
    FOR    ${i}    IN RANGE    4
        IF    ${i} == 1
            Log    warning iteration    WARN
        ELSE
            Nested Keyword
        END
    END
    ${n}=    Set Variable    ${0}
    WHILE    ${n} < 4
        ${n}=    Evaluate    ${n} + 1
        IF    ${n} == 2
            Log    error iteration    ERROR
        END
        Log    while ${n}
    END

WUKS Success
    Wait Until Keyword Succeeds    5x    1ms    Attempt    4

WUKS Warning
    Wait Until Keyword Succeeds    5x    1ms    Attempt    4    2

WUKS Failure
    Wait Until Keyword Succeeds    4x    1ms    Attempt    10    2

Skipped Body
    Skip    skipped body
    Nested Keyword

*** Keywords ***
Fixture With Teardown
    [Documentation]    fixture doc
    Log    fixture body
    [Teardown]    Log    fixture own teardown

Nested Keyword
    [Documentation]    nested doc & <literal> with *bold* and _italic_
    Log    nested trace    TRACE
    Log    nested info
    [Teardown]    Log    nested teardown

Attempt
    [Arguments]    ${succeed_at}    ${warn_at}=0
    ${attempt}=    Get Variable Value    ${ATTEMPT_NUMBER}    ${0}
    ${attempt}=    Evaluate    ${attempt} + 1
    Set Test Variable    ${ATTEMPT_NUMBER}    ${attempt}
    IF    ${attempt} == ${warn_at}
        Log    retry warning    WARN
    END
    Should Be True    ${attempt} >= ${succeed_at}
