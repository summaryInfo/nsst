if [[ -o interactive ]] && [ -z "$_NSST_INTEGRATION_INITALIZED" ]; then
    _NSST_INTEGRATION_INITALIZED=1

    # Stop printing end of the line after command,
    # nsst will do it better for us
    set ++le-promptsp

    _nsst_prompt_start() { printf "\033]133;A\a"; }
    _nsst_prompt_end() { printf "\033]133;B\a"; }
    _nsst_command_start() { printf "\033]133;C\a"; }
    _nsst_command_end() { printf "\033]133;D\a"; }
    _nsst_report_cwd() { printf "\033]7;file://$HOSTNAME$PWD\a" > /dev/tty; }

    # FIXME It's not entirely correct to compare versions like this
    if [  "$YASH_VERSION" '>' "2.56.1" ]; then
        _nsst_prompt_hook() {
            if [ -n "$_NSST_COMMAND_EXECUTED" ]; then
                _NSST_COMMAND_EXECUTED=
                _nsst_command_end
            fi
            if [ "${_NSST_MODIFIED_PS1-}" != "$YASH_PS1" ]; then
                _NSST_SAVED_PS1="$YASH_PS1"
                [[ "$YASH_PS1" != *"$(_nsst_prompt_start)"* ]] && \
                    YASH_PS1='\['"$(_nsst_prompt_start)"'\]'"$YASH_PS1"
                [[ "$YASH_PS1" != *"$(_nsst_prompt_end)"* ]] && \
                    YASH_PS1="$YASH_PS1"'\['"$(_nsst_prompt_end)"'\]'
                _NSST_MODIFIED_PS1="$YASH_PS1"
            fi
        }

        _nsst_precmd_hook() {
            YASH_PS1="$_NSST_SAVED_PS1"
            _nsst_command_start
            _NSST_COMMAND_EXECUTED=1
        }

        POST_PROMPT_COMMAND=("$POST_PROMPT_COMMAND" '_nsst_precmd_hook')
    else
        _nsst_prompt_hook() {
            if [ "${_NSST_MODIFIED_PS1-}" != "$YASH_PS1" ]; then
                [[ "$YASH_PS1" != *"$(_nsst_prompt_start)"* ]] && \
                    YASH_PS1='\['"$(_nsst_prompt_start)"'\]'"$YASH_PS1"
                [[ "$YASH_PS1" != *"$(_nsst_prompt_end)"* ]] && \
                    YASH_PS1="$YASH_PS1"'\['"$(_nsst_prompt_end)"'\]'
                _NSST_MODIFIED_PS1="$YASH_PS1"
            fi
            if [ "${_NSST_MODIFIED_PS1R-}" != "$YASH_PS1R" ]; then
                [[ "$YASH_PS1R" != *"$(_nsst_command_start)"* ]] && \
                    YASH_PS1R="$YASH_PS1R"'\['"$(_nsst_command_start)"'\]'
                _NSST_MODIFIED_PS1R="$YASH_PS1R"
            fi
        }
    fi

    PROMPT_COMMAND=("$PROMPT_COMMAND" '_nsst_prompt_hook')
    YASH_AFTER_CD=("$YASH_AFTER_CD" '_nsst_report_cwd')

    _nsst_report_cwd
fi
