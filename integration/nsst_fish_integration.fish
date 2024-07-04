if status --is-interactive; and not functions -q _nsst_prompt_start
    function _nsst_prompt_start; printf "\033]133;A\a"; end
    function _nsst_prompt_end; printf "\033]133;B\a"; end
    function _nsst_command_start; printf "\033]133;C\a"; end
    function _nsst_command_end; printf "\033]133;D\a"; end
    function _nsst_report_cwd; printf "\033]7;file://$HOSTNAME$PWD\a" > /dev/tty; end

    # Configure shell for avoid redrawing prompt on resize
    set -g fish_handle_reflow 0

    function _nsst_preexec_hook --on-event fish_preexec
        _nsst_command_start
    end
    function _nsst_postexec_hook --on-event fish_postexec
        _nsst_command_end
    end
    function _nsst_prompt_hook --on-event fish_prompt
        _nsst_prompt_start
    end

    # There's no prompt end hook, so we have to decorate the function
    functions -c fish_prompt _nsst_old_fish_prompt
    function fish_prompt
        _nsst_old_fish_prompt
        _nsst_prompt_end
    end

    function _nsst_cwd_hook --on-variable PWD
        _nsst_report_cwd
    end

    _nsst_report_cwd
end
