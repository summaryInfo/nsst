if [[ -o interactive ]] && [ -z "$_NSST_INTEGRATION_INITALIZED" ]; then
    _NSST_INTEGRATION_INITALIZED=1

    _nsst_prompt_start() { printf "\033]133;A\a" }
    _nsst_prompt_end() { printf "\033]133;B\a" }
    _nsst_command_start() { printf "\033]133;C\a" }
    _nsst_command_end() { printf "\033]133;D\a" }

    _nsst_precmd_hook() {
        if [ -n "${_NSST_COMMAND_EXECUTED-}" ]; then
            _NSST_COMMAND_EXECUTED=
            _nsst_command_end
        fi
        if [ "$PS1" != "${_NSST_MODIFIED_PS1-}" ]; then
            _NSST_SAVED_PS1="$PS1"
            [[ "$PS1" != *"$(_nsst_prompt_start)"* ]] && \
                PS1='%{'"$(_nsst_prompt_start)"'%}'"$PS1"
            [[ "$PS1" != *"$(_nsst_prompt_end)"* ]] && \
                PS1="$PS1"'%{'"$(_nsst_prompt_end)"'%}'
            _NSST_MODIFIED_PS1="$PS1"
        fi
    }

    _nsst_preexec_hook() {
        PS1="$_NSST_SAVED_PS1"
        _NSST_COMMAND_EXECUTED=1
        _nsst_command_start
    }

    [[ -z ${precmd_functions-} ]] && precmd_functions=()
    precmd_functions=($precmd_functions _nsst_precmd_hook)

    [[ -z ${preexec_functions-} ]] && preexec_functions=()
    preexec_functions=($preexec_functions _nsst_preexec_hook)

    _NSST_COMMAND_EXECUTED=

    # This is not set by default since oh-my-zsh and others can do it
    # by themselves, if you need that one et _NSST_NEED_CD_HOOK to 1.
    if [[ -n "$_NSST_NEED_CD_HOOK" ]]; then
        _nsst_report_cwd() {
            printf "\033]7;file://$HOSTNAME$PWD\a" > /dev/tty;
        }

        [[ -z ${chpwd_functions-} ]] && chpwd_functions=()
        chpwd_functions=($preexec_functions _nsst_report_cwd)
        _nsst_report_cwd
    fi
fi
