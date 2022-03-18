set expandtab
set tabstop=8
set softtabstop=2
set shiftwidth=2

autocmd BufNewFile,BufRead *.c,*.cxx,*.h set textwidth=100

" This is the same as the default minus 0{, which is really annoying otherwise
" with our coding style.
set cinkeys=0},0),:,0#,!^F,o,O,e
