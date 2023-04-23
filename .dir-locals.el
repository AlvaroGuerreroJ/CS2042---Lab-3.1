;;; Directory Local Variables
;;; For more information see (info "(emacs) Directory Variables")

((c++-mode . ((fill-column . 96)
              ;; clang-tools (including clangd) is version 13 and we are using
              ;; version 14 configuration options, this makes it so it calls
              ;; clang-format which is provided by clang_14
              (+format-with-lsp . nil))))
