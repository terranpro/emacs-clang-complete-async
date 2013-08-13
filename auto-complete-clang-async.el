;;; auto-complete-clang-async-once.el --- Auto Completion source for clang for GNU Emacs

;; Copyright (C) 2010  Brian Jiang
;; Copyright (C) 2012  Taylan Ulrich Bayirli/Kammer

;; Authors: Brian Jiang <brianjcj@gmail.com>
;;          Golevka(?) [https://github.com/Golevka]
;;          Taylan Ulrich Bayirli/Kammer <taylanbayirli@gmail.com>
;;          Cjacker <cjacker@gmail.com>
;;          Many others
;; Keywords: completion, convenience
;; Version: 0

;; This program is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with this program.  If not, see <http://www.gnu.org/licenses/>.


;;; Commentary:

;; Auto Completion source for clang.
;; Uses a "completion server" process to utilize libclang.
;; Also provides flymake syntax checking.
;; Cjacker: Made clang-complete server only run once for one emacs instance, and change the source file internally
;;; Code:


(provide 'auto-complete-clang-async)
(require 'cl)
(require 'auto-complete)
(require 'flymake)


(defcustom ac-clang-complete-executable
  (executable-find "clang-complete")
  "Location of clang-complete executable."
  :group 'auto-complete
  :type 'file)

(defcustom ac-clang-lang-option-function nil
  "Function to return the lang type for option -x."
  :group 'auto-complete
  :type 'function)

(defcustom ac-clang-cflags nil
  "Extra flags to pass to the Clang executable.
This variable will typically contain include paths, e.g., (\"-I~/MyProject\" \"-I.\")."
  :group 'auto-complete
  :type '(repeat (string :tag "Argument" "")))
(make-variable-buffer-local 'ac-clang-cflags)

(defvar ac-clang-project-srcs nil)
(make-variable-buffer-local 'ac-clang-project-srcs)

(defvar ac-clang-project-id nil)
(make-variable-buffer-local 'ac-clang-project-id)

(defvar ac-clang-project-directory nil)
(make-variable-buffer-local 'ac-clang-project-directory)

(defun ac-clang-set-cflags ()
  "Set `ac-clang-cflags' interactively."
  (interactive)
  (setq ac-clang-cflags (split-string (read-string "New cflags: ")))
  (ac-clang-update-cmdlineargs))

(defun ac-clang-set-cflags-from-shell-command ()
  "Set `ac-clang-cflags' to a shell command's output.  Set new
cflags for ac-clang from shell command output"
  (interactive)
  (setq ac-clang-cflags
        (split-string
         (shell-command-to-string
          (read-shell-command "Shell command: " nil nil
                              (and buffer-file-name
                                   (file-relative-name buffer-file-name))))))
  (ac-clang-update-cmdlineargs))

(defvar ac-clang-prefix-header nil
  "The prefix header to pass to the Clang executable.")
(make-variable-buffer-local 'ac-clang-prefix-header)

(defun ac-clang-set-prefix-header (prefix-header)
  "Set `ac-clang-prefix-header' interactively."
  (interactive
   (let ((default (car (directory-files "." t "\\([^.]h\\|[^h]\\).pch\\'" t))))
     (list
      (read-file-name (concat "Clang prefix header (currently " (or ac-clang-prefix-header "nil") "): ")
                      (when default (file-name-directory default))
                      default nil (when default (file-name-nondirectory default))))))
  (cond
   ((string-match "^[\s\t]*$" prefix-header)
    (setq ac-clang-prefix-header nil))
   (t
    (setq ac-clang-prefix-header prefix-header))))


(defconst ac-clang-completion-pattern
  "^COMPLETION: \\(%s[^\s\n:]*\\)\\(?: : \\)*\\(.*$\\)")

(defun ac-clang-parse-output (prefix)
  (goto-char (point-min))
  (let ((pattern (format ac-clang-completion-pattern
                         (regexp-quote prefix)))
        lines match detailed-info
        (prev-match ""))
    (while (re-search-forward pattern nil t)
      (setq match (match-string-no-properties 1))
      (unless (string= "Pattern" match)
        (setq detailed-info (match-string-no-properties 2))

        (if (string= match prev-match)
            (progn
              (when detailed-info
                (setq match (propertize match
                                        'ac-clang-help
                                        (concat
                                         (get-text-property 0 'ac-clang-help (car lines))
                                         "\n"
                                         detailed-info)))
                (setf (car lines) match)
                ))
          (setq prev-match match)
          (when detailed-info
            (setq match (propertize match 'ac-clang-help detailed-info)))
          (push match lines))))
    lines))


(defconst ac-clang-error-buffer-name "*clang error*")

(defun ac-clang-handle-error (res args)
  (goto-char (point-min))
  (let* ((buf (get-buffer-create ac-clang-error-buffer-name))
         (cmd (concat ac-clang-complete-executable
		      " " (mapconcat 'identity args " ")))
         (pattern (format ac-clang-completion-pattern ""))
         (err (if (re-search-forward pattern nil t)
                  (buffer-substring-no-properties (point-min)
                                                  (1- (match-beginning 0)))
                ;; Warn the user more agressively if no match was found.
                (message "clang failed with error %d:\n%s" res cmd)
                (buffer-string))))

    (with-current-buffer buf
      (let ((inhibit-read-only t))
        (erase-buffer)
        (insert (current-time-string)
                (format "\nclang failed with error %d:\n" res)
                cmd "\n\n")
        (insert err)
        (setq buffer-read-only t)
        (goto-char (point-min))))))

(defun ac-clang-call-process (prefix &rest args)
  (let ((buf (get-buffer-create "*clang-output*"))
        res)
    (with-current-buffer buf (erase-buffer))
    (setq res (apply 'call-process-region (point-min) (point-max)
                     ac-clang-complete-executable nil buf nil args))
    (with-current-buffer buf
      (unless (eq 0 res)
        (ac-clang-handle-error res args))
      ;; Still try to get any useful input.
      (ac-clang-parse-output prefix))))


(defsubst ac-clang-create-position-string (pos)
  (save-excursion
    (goto-char pos)
    (format "row:%d\ncolumn:%d\nprefix:%s\n"
            (line-number-at-pos)
            (1+ (- (point) (line-beginning-position)))
	    (or ac-prefix ""))))

(defsubst ac-clang-lang-option ()
  (or (and ac-clang-lang-option-function
           (funcall ac-clang-lang-option-function))
      (cond ((eq major-mode 'c++-mode)
             "c++")
            ((eq major-mode 'c-mode)
             "c")
            ((eq major-mode 'objc-mode)
             (cond ((string= "m" (file-name-extension (buffer-file-name)))
                    "objective-c")
                   (t
                    "objective-c++")))
            (t
             "c++"))))

(defsubst ac-clang-build-complete-args ()
  (append '("-cc1" "-fsyntax-only")
          (list "-x" (ac-clang-lang-option))
          ac-clang-cflags
          (when (stringp ac-clang-prefix-header)
            (list "-include-pch" (expand-file-name ac-clang-prefix-header)))))


(defsubst ac-clang-clean-document (s)
  (when s
    (setq s (replace-regexp-in-string "<#\\|#>\\|\\[#" "" s))
    (setq s (replace-regexp-in-string "#\\]" " " s)))
  s)

(defun ac-clang-document (item)
  (if (stringp item)
      (let (s)
        (setq s (get-text-property 0 'ac-clang-help item))
        (ac-clang-clean-document s)))
  ;; (popup-item-property item 'ac-clang-help)
  )


(defface ac-clang-candidate-face
  '((t (:background "lightgray" :foreground "navy")))
  "Face for clang candidate"
  :group 'auto-complete)

(defface ac-clang-selection-face
  '((t (:background "navy" :foreground "white")))
  "Face for the clang selected candidate."
  :group 'auto-complete)

(defsubst ac-clang-in-string/comment ()
  "Return non-nil if point is in a literal (a comment or string)."
  (nth 8 (syntax-ppss)))


(defvar ac-clang-template-start-point nil)
(defvar ac-clang-template-candidates (list "ok" "no" "yes:)"))

(defun ac-clang-action ()
  (interactive)
  ;; (ac-last-quick-help)
  (let ((help (ac-clang-clean-document (get-text-property 0 'ac-clang-help (cdr ac-last-completion))))
        (raw-help (get-text-property 0 'ac-clang-help (cdr ac-last-completion)))
        (candidates (list)) ss fn args (ret-t "") ret-f)
    (setq ss (split-string raw-help "\n"))
    (dolist (s ss)
      (when (string-match "\\[#\\(.*\\)#\\]" s)
        (setq ret-t (match-string 1 s)))
      (setq s (replace-regexp-in-string "\\[#.*?#\\]" "" s))
      (cond ((string-match "^\\([^(<]*\\)\\(:.*\\)" s)
             (setq fn (match-string 1 s)
                   args (match-string 2 s))
             (push (propertize (ac-clang-clean-document args) 'ac-clang-help ret-t
                               'raw-args args) candidates))
            ((string-match "^\\([^(]*\\)\\((.*)\\)" s)
             (setq fn (match-string 1 s)
                   args (match-string 2 s))
             (push (propertize (ac-clang-clean-document args) 'ac-clang-help ret-t
                               'raw-args args) candidates)
             (when (string-match "\{#" args)
               (setq args (replace-regexp-in-string "\{#.*#\}" "" args))
               (push (propertize (ac-clang-clean-document args) 'ac-clang-help ret-t
                                 'raw-args args) candidates))
             (when (string-match ", \\.\\.\\." args)
               (setq args (replace-regexp-in-string ", \\.\\.\\." "" args))
               (push (propertize (ac-clang-clean-document args) 'ac-clang-help ret-t
                                 'raw-args args) candidates)))
            ((string-match "^\\([^(]*\\)(\\*)\\((.*)\\)" ret-t) ;; check whether it is a function ptr
             (setq ret-f (match-string 1 ret-t)
                   args (match-string 2 ret-t))
             (push (propertize args 'ac-clang-help ret-f 'raw-args "") candidates)
             (when (string-match ", \\.\\.\\." args)
               (setq args (replace-regexp-in-string ", \\.\\.\\." "" args))
               (push (propertize args 'ac-clang-help ret-f 'raw-args "") candidates)))))
    (cond (candidates
           (setq candidates (delete-dups candidates))
           (setq candidates (nreverse candidates))
           (setq ac-clang-template-candidates candidates)
           (setq ac-clang-template-start-point (point))
           (ac-complete-clang-template)

           (unless (cdr candidates) ;; unless length > 1
             (message (replace-regexp-in-string "\n" "   ;    " help))))
          (t
           (message (replace-regexp-in-string "\n" "   ;    " help))))))

(defun ac-clang-prefix ()
  (or (ac-prefix-symbol)
      (let ((c (char-before)))
        (when (or (eq ?\. c)
                  ;; ->
                  (and (eq ?> c)
                       (eq ?- (char-before (1- (point)))))
                  ;; ::
                  (and (eq ?: c)
                       (eq ?: (char-before (1- (point))))))
          (point)))))

(defun ac-clang-same-count-in-string (c1 c2 s)
  (let ((count 0) (cur 0) (end (length s)) c)
    (while (< cur end)
      (setq c (aref s cur))
      (cond ((eq c1 c)
             (setq count (1+ count)))
            ((eq c2 c)
             (setq count (1- count))))
      (setq cur (1+ cur)))
    (= count 0)))

(defun ac-clang-split-args (s)
  (let ((sl (split-string s ", *")))
    (cond ((string-match "<\\|(" s)
           (let ((res (list)) (pre "") subs)
             (while sl
               (setq subs (pop sl))
               (unless (string= pre "")
                 (setq subs (concat pre ", " subs))
                 (setq pre ""))
               (cond ((and (ac-clang-same-count-in-string ?\< ?\> subs)
                           (ac-clang-same-count-in-string ?\( ?\) subs))
                      ;; (cond ((ac-clang-same-count-in-string ?\< ?\> subs)
                      (push subs res))
                     (t
                      (setq pre subs))))
             (nreverse res)))
          (t
           sl))))


(defun ac-clang-template-candidate ()
  ac-clang-template-candidates)

(defun ac-clang-template-action ()
  (interactive)
  (unless (null ac-clang-template-start-point)
    (let ((pos (point)) sl (snp "")
          (s (get-text-property 0 'raw-args (cdr ac-last-completion))))
      (cond ((string= s "")
             ;; function ptr call
             (setq s (cdr ac-last-completion))
             (setq s (replace-regexp-in-string "^(\\|)$" "" s))
             (setq sl (ac-clang-split-args s))
             (cond ((featurep 'yasnippet)
                    (dolist (arg sl)
                      (setq snp (concat snp ", ${" arg "}")))
                    (condition-case nil
                        (yas/expand-snippet (concat "("  (substring snp 2) ")")
                                            ac-clang-template-start-point pos) ;; 0.6.1c
                      (error
                       ;; try this one:
                       (ignore-errors (yas/expand-snippet
                                       ac-clang-template-start-point pos
                                       (concat "("  (substring snp 2) ")"))) ;; work in 0.5.7
                       )))
                   ((featurep 'snippet)
                    (delete-region ac-clang-template-start-point pos)
                    (dolist (arg sl)
                      (setq snp (concat snp ", $${" arg "}")))
                    (snippet-insert (concat "("  (substring snp 2) ")")))
                   (t
                    (message "Dude! You are too out! Please install a yasnippet or a snippet script:)"))))
            (t
             (unless (string= s "()")
               (setq s (replace-regexp-in-string "{#" "" s))
               (setq s (replace-regexp-in-string "#}" "" s))
               (cond ((featurep 'yasnippet)
                      (setq s (replace-regexp-in-string "<#" "${" s))
                      (setq s (replace-regexp-in-string "#>" "}" s))
                      (setq s (replace-regexp-in-string ", \\.\\.\\." "}, ${..." s))
                      (condition-case nil
                          (yas/expand-snippet s ac-clang-template-start-point pos) ;; 0.6.1c
                        (error
                         ;; try this one:
                         (ignore-errors (yas/expand-snippet ac-clang-template-start-point pos s)) ;; work in 0.5.7
                         )))
                     ((featurep 'snippet)
                      (delete-region ac-clang-template-start-point pos)
                      (setq s (replace-regexp-in-string "<#" "$${" s))
                      (setq s (replace-regexp-in-string "#>" "}" s))
                      (setq s (replace-regexp-in-string ", \\.\\.\\." "}, $${..." s))
                      (snippet-insert s))
                     (t
                      (message "Dude! You are too out! Please install a yasnippet or a snippet script:)")))))))))


(defun ac-clang-template-prefix ()
  ac-clang-template-start-point)


;; This source shall only be used internally.
(ac-define-source clang-template
  '((candidates . ac-clang-template-candidate)
    (prefix . ac-clang-template-prefix)
    (requires . 0)
    (action . ac-clang-template-action)
    (document . ac-clang-document)
    (cache)
    (symbol . "t")))


;;;
;;; Rest of the file is related to async.
;;;

(defvar ac-clang-status 'idle)
(defvar ac-clang-current-candidate nil)
(defvar ac-clang-completion-process nil)
(defvar ac-clang-saved-prefix "")

(defvar current-clang-file "")

;;(make-variable-buffer-local 'ac-clang-status)
;;(make-variable-buffer-local 'ac-clang-current-candidate)
;;(make-variable-buffer-local 'ac-clang-completion-process)

;;;
;;; Functions to speak with the clang-complete process
;;;

(defun ac-clang-send-source-code (proc)
  (save-restriction
    (widen)
    (process-send-string 
     proc (format "source_length:%d\n" 
                  (length (string-as-unibyte   ; fix non-ascii character problem
                           (buffer-substring-no-properties (point-min) (point-max)))
                          )))
    (process-send-string proc (buffer-substring-no-properties (point-min) (point-max)))
    (process-send-string proc "\n\n")))

(defun ac-clang-send-reparse-request (proc)
  (if (eq (process-status "clang-complete") 'run)
      (save-restriction
	(widen)
	(process-send-string proc "SOURCEFILE\n")
	(ac-clang-send-source-code proc)
	(process-send-string proc "REPARSE\n\n"))))

(defun ac-clang-send-completion-request (proc)
  (if (not (string= current-clang-file (buffer-file-name)))
      (ac-clang-filechanged))
  (save-restriction
    (widen)
    (process-send-string proc "COMPLETION\n")
    (process-send-string proc (ac-clang-create-position-string (- (point) (length ac-prefix))))
    (ac-clang-send-source-code proc)))

(defun ac-clang-project-add-src (proc src)
  (with-current-buffer (process-buffer proc)
    (erase-buffer))

  (message (format "Adding %s to Clang Project..." src))

  (setq ac-clang-status 'prj-srcs)
  (with-current-buffer (get-file-buffer current-clang-file)
   (process-send-string 
    proc 
    (format "PROJECT\nADD_SRC\nPROJECTID:%d\n%s\n" 
	    ac-clang-project-id src))))

(defvar ac-clang-project-srcs-queue nil)

(defun ac-clang-project-add-srcs (&optional proc)
  (interactive)
  (when ac-clang-project-srcs
    (unless proc 
      (setq proc ac-clang-completion-process))

    (setq ac-clang-project-srcs-queue ac-clang-project-srcs)
    (ac-clang-project-add-src proc (pop ac-clang-project-srcs-queue))))

(defun ac-clang-project-locate (&optional proc)
  (interactive)
  (unless proc 
    (setq proc ac-clang-completion-process))

  (setq current-clang-file (buffer-file-name))

  (setq ac-clang-status 'prj-locate)
  (with-current-buffer (process-buffer proc)
    (erase-buffer))

  (process-send-string proc (format "PROJECT\nLOCATE\nPROJECTID:%d\n" 
				    ac-clang-project-id))
  (process-send-string proc (format "src:%s\n" (buffer-file-name)))
  (process-send-string 
   proc 
   (ac-clang-create-position-string (- (point) (length ac-prefix)))))

(defun ac-clang-project-find-id (&optional proc)
  (interactive)
  (unless proc 
    (setq proc ac-clang-completion-process))

  (when proc 
    (message "Finding ID...")

    (setq current-clang-file (buffer-file-name))
    (setq ac-clang-status 'prj-id)
    (with-current-buffer (process-buffer proc)
      (erase-buffer))
    (process-send-string 
     proc 
     (format "PROJECT\nFIND_ID\n%s\n" current-clang-file))))

(defun ac-clang-project-new (&optional proc)
  (interactive)
  (unless proc 
    (setq proc ac-clang-completion-process))

  (message "Creating New Clang Project")

  (setq current-clang-file (buffer-file-name))
  (setq ac-clang-status 'prj-id)
  (with-current-buffer (process-buffer proc)
    (erase-buffer))
  (process-send-string proc "PROJECT\nNEW\n"))

(defun ac-clang-project-send-options (&optional proc)
  (interactive)
  (unless proc 
    (setq proc ac-clang-completion-process))

  (message "Sending Options...")

  (process-send-string 
   proc 
   (format "PROJECT\nOPTIONS\nPROJECTID:%d\n" ac-clang-project-id))

  (mapc
   (lambda (arg)
     (process-send-string proc (format "%s " arg)))
   (ac-clang-build-complete-args))
  (process-send-string proc "\n"))

(defun ac-clang-project-testing ()
  (interactive)
  (ac-clang-relaunch-completion-process)
  (ac-clang-project-new)
  (ac-clang-project-send-options)
  (ac-clang-project-add-srcs))

(defun ac-clang-send-location-request (&optional proc)
  (interactive)
  (if (not (string= current-clang-file (buffer-file-name)))
      (ac-clang-filechanged))
  (unless proc 
    (setq proc ac-clang-completion-process))

  (if (not (string= current-clang-file (buffer-file-name)))
      (ac-clang-filechanged))
  (with-current-buffer (process-buffer proc)
    (erase-buffer))
  (save-restriction
    (widen)
    (setq ac-clang-status 'locate)
    (message "Locating...")
    (process-send-string proc "LOCATE\n")
    (process-send-string proc (ac-clang-create-position-string (- (point) (length ac-prefix))))
    (ac-clang-send-source-code proc)))

(defun ac-clang-send-syntaxcheck-request (proc)
  (save-restriction
    (widen)
    (process-send-string proc "SYNTAXCHECK\n")
    (ac-clang-send-source-code proc)))

(defun ac-clang-send-cmdline-args (proc)
  ;; send message head and num_args
  (process-send-string proc "CMDLINEARGS\n")
  (process-send-string
   proc (format "num_args:%d\n" (length (ac-clang-build-complete-args))))

  ;; send arguments
  (mapc
   (lambda (arg)
     (process-send-string proc (format "%s " arg)))
   (ac-clang-build-complete-args))
  (process-send-string proc "\n"))

(defun ac-clang-update-cmdlineargs ()
  (interactive)
  (if (listp ac-clang-cflags)
         (ac-clang-send-cmdline-args ac-clang-completion-process)
         (message "`ac-clang-cflags' should be a list of strings")))

(defun ac-clang-send-filechanged (proc)
  ;; send message head and num_args
  (process-send-string proc "FILECHANGED\n")
  (process-send-string
   proc (format "filename:%s\n" (buffer-file-name)))
  (process-send-string
   proc (format "num_args:%d\n" (length (ac-clang-build-complete-args))))
  ;; send arguments
  (mapc
   (lambda (arg)
     (process-send-string proc (format "%s " arg)))
   (ac-clang-build-complete-args))
  (process-send-string proc "\n"))

(defun ac-clang-filechanged ()
  (interactive)
  (if (not (string= current-clang-file (buffer-file-name)))
    (setq current-clang-file (buffer-file-name)))
  (ac-clang-send-filechanged ac-clang-completion-process))


(defun ac-clang-send-shutdown-command (proc)
  (if (eq (process-status "clang-complete") 'run)
    (process-send-string proc "SHUTDOWN\n"))
  )


(defun ac-clang-append-process-output-to-process-buffer (process output)
  "Append process output to the process buffer."
  (with-current-buffer (process-buffer process)
    (save-excursion
      ;; Insert the text, advancing the process marker.
      (goto-char (process-mark process))
      (insert output)
      (set-marker (process-mark process) (point)))
    (goto-char (process-mark process))))


;;
;;  Receive server responses (completion candidates) and fire auto-complete
;;
(defun ac-clang-parse-completion-results (proc)
  (with-current-buffer (process-buffer proc)
    (ac-clang-parse-output ac-clang-saved-prefix)))

(defun ac-clang-project-new-parse-result (proc)
  (with-current-buffer (get-file-buffer current-clang-file)
    (setq ac-clang-project-id 
	  (with-current-buffer (process-buffer proc)
	    (let ((result)
		  (regexp (rx "PROJECTID:"
			      (group (one-or-more (not (any ?\n)))) ?\n)))
	      (goto-char (point-min))
	      (when (re-search-forward regexp nil t)
		(string-to-number (match-string 1))))))
    ;; TODO incase shit happens w/prj find
    ;; (if (< ac-clang-project-id 0)
    ;; 	(ac-clang-project-new proc))

))

(defun ac-clang-parse-location-results (proc)
  "Returns a list containing the file, line, and column of the last LOCATE query."
  (with-current-buffer (process-buffer proc)
    (let ((result)
	  (regexp (rx "LOCATE:" (zero-or-more not-newline) ?\n
		      "file:" (group (one-or-more (not (any ?\n)))) ?\n
		      "line:" (group (one-or-more (not (any ?\n)))) ?\n
		      "column:" (group (one-or-more (not (any ?\n)))))))
      (goto-char (point-min))
      (when (re-search-forward regexp nil t)
	(setq result 
	      (list (match-string 1) 
	       (string-to-number (match-string 2))
	       (string-to-number (match-string 3)))))
      result)))

(defun ac-clang-project-locate-preview-src-face-attr-add (txt attr value)
  (let* ((cur 0)
	 (end (length txt))
	 (iter 0)
	 (curface)
	 (newbg (face-attribute 'popup-tip-face :background))
	 (next-change 0))
    ;(pp txt)
    (while (and (< next-change end)
		(< iter 10000))
      (setq iter (1+ iter))
      (setq next-change ;(next-property-change cur txt end)
	    (next-single-property-change cur 'face txt end))
      (setq curface (or (get-text-property cur 'face txt) 'popup-tip-face))

      (setq curface (if (listp curface)
			(car curface)
		      curface))
      
      (setq curface 
      	    (if (listp curface)
      		(append curface (list attr value))
      	      (list curface attr value)))



      (set-text-properties cur next-change nil txt)
      ;(remove-text-properties cur next-change txt)
      (add-text-properties cur next-change
			   `(face ,curface)
			   ;`(face (,curface ,attr ,value )) txt
			   txt)
      ;; (pp (substring txt cur next-change))
      ;; (message (format "%d %d" 
      ;; 		       cur next-change))
      (setq cur next-change))
    ;(pp txt)
    txt))

(defun brian-face-attr-add-tester ()
  (interactive)
  (let ((txt (buffer-substring (point-min) (point-max))))
    (with-current-buffer (get-buffer-create "garbage")
      (insert 
       (ac-clang-project-locate-preview-src-face-attr-add
	txt :background "khaki1")))))

(defun ac-clang-project-locate-preview-src (file line col &optional offset)
  (save-window-excursion
   (with-current-buffer (find-file file) ;(find-file-noselect file)
     (font-lock-fontify-buffer)
     (save-restriction
       (goto-char (point-min))
       (forward-line (1- line))

       (let* ((popup-tip-face-bg (face-attribute 'popup-tip-face :background))
	      (off (or offset 2))
	      (hit-text (propertize
			 (buffer-substring-no-properties
			  (point-at-bol) (point-at-eol))
			 'face '(popup-tip-face bold)))

	      (pre-text 
	       (buffer-substring-no-properties
		(save-excursion (progn (forward-line (- off))
				       (point)))
		(point-at-bol)))

	      (post-text
	       (buffer-substring-no-properties
		(point-at-eol)
		(save-excursion (progn (forward-line off)
				       (point))))))

	 ;; (ac-clang-project-locate-preview-src-face-attr-add
	 ;;  (concat pre-text hit-text post-text)
	 ;;  :background
	 ;;  popup-tip-face-bg)

	 (concat pre-text hit-text post-text))))))

;; (let ((popup-tip-face-bg (face-attribute 'popup-tip-face :background)))
;;   (propertize 
;;   (buffer-substring		       ;buffer-substring-no-properties
;;    (point-min) (point-max))
;;   'face `(:background ,popup-tip-face-bg)))

;; (let* ((txt  (buffer-substring (point-min) (+ (point-min) 25)))
;;        (cur 1)
;;        (end (length txt))
;;        (next-change (next-property-change cur txt end)))
;;   (while (setq next-change (next-property-change cur txt end))
;;     (add-text-properties cur next-change '(face (:background blue)))
;;     (setq cur next-change))
;;   txt)

(defvar ac-clang-project-locate-menu nil)
(defvar ac-clang-project-locate-menu-doc nil)

(defun ac-clang-project-show-quick-help (parentmenu)
  (let* ((doc (popup-menu-documentation parentmenu))
	 ;(doc (substring-no-properties (popup-menu-documentation parentmenu)))
	 (max-width (window-width))
	 (doc-filled (popup-fill-string doc nil max-width 'left t))
	 (width (min max-width (car doc-filled)))
	 (height (min 30 (length (cdr doc-filled))))
	 (ovbeg 
	  (overlay-start (popup-line-overlay parentmenu 0)))
	 (pt (let (
		   ;; (ovend (overlay-end
		   ;; 	   (popup-line-overlay
		   ;; 	    parentmenu
		   ;; 	    (or (popup-offset parentmenu)
		   ;; 		(popup-selected-line parentmenu)))))
		   
		   (ovend 
		    (overlay-end (popup-line-overlay 
				  parentmenu 
				  (1- 
				   (length (popup-overlays parentmenu)))))))

	       ;(message (format "ovbeg %d ovend %d" ovbeg ovend))
	       (goto-char ovend)
	       
	       ;(forward-line (1+ height))
	       (forward-line 1)
	       (move-to-column 0)
	       ;(message (format "end of parentmenu = %d" (point)))
	       (point)))
	 (row (line-number-at-pos pt))
	 (direction (popup-calculate-direction height row))
	 (top (save-excursion 
		(goto-char ovbeg)
		(forward-line (- 1))
		(point)))
	 (finalpt (if (eq direction -1)
		      (save-excursion 
			(goto-char top)
			(vertical-motion (- height))
			(point))
		    pt))
	 (menu (popup-create finalpt width height
			     :face 'popup-tip-face
			     :around nil
			     :scroll-bar t
					;:parent parentmenu
					;:parent-offset (popup-offset parentmenu)
			     )))
    
    ;(pp (popup-list parentmenu))
    ;(pp (popup-overlays parentmenu))
    ;(pp doc-filled)


    ;; (setq doc-filled
    ;; 	  (list
    ;; 	   (car doc-filled)
    ;; 	   (mapcar 
    ;; 	    #'(lambda (str)
    ;; 		(ac-clang-project-locate-preview-src-face-attr-add
    ;; 		 str
    ;; 		 :background "khaki1"))

    ;; 	    (remove-if
    ;; 	     #'(lambda (str) (string= "" str))
    ;; 	     (cdr doc-filled)   ))))

;    (pp doc-filled)

;;     (message 
;;      (format "direction = %d height = %d
;; parentheight = %d top = %d pt = %d finalpt = %d" 
;; 	     direction height (popup-current-height parentmenu) 
;; 	     top pt finalpt))
    (popup-set-list menu (cdr doc-filled))
    (popup-draw menu)
    (clear-this-command-keys)
    (push (read-event prompt) unread-command-events)
    (popup-delete menu)))

(defun ac-clang-project-locate-popup (results)
  (save-window-excursion
    (with-current-buffer (get-file-buffer current-clang-file)
     (let (;(pt (point))
	   (cont "...")
	   (file-width 20)
	   (pop-data)
	   (map (copy-keymap popup-menu-keymap))
	   (pop))

       (define-key map (kbd "<up>") #'(lambda ()
					(interactive)
					(popup-previous
					 ac-clang-project-locate-menu)
					;; (popup-tip
					;;  (popup-menu-documentation 
					;;   ac-clang-project-locate-menu))
					
					(ac-clang-project-show-quick-help
					 ac-clang-project-locate-menu)))

       (define-key map (kbd "<down>") #'(lambda ()
					  (interactive)
					  (popup-next 
					   ac-clang-project-locate-menu)
					  ;; (popup-tip
					  ;;  (popup-menu-documentation 
					  ;;   ac-clang-project-locate-menu))
					
					  (ac-clang-project-show-quick-help
					   ac-clang-project-locate-menu)))

       (setq pop-data
	     (loop for (desc file line col def) in results
		   for summary = 
		   (format
		    (concat "%12s ! %"
			    (number-to-string file-width)
			    "s :%4d:%3d")
		    desc
		    (if (> (length file) file-width)
			(concat cont (subseq file (+ (length cont)
						     (- 0 file-width))))
		      file)
		    line
		    col)
		   collect
		   (popup-make-item
		    summary
		    :value (list file line col)
		    :document (ac-clang-project-locate-preview-src 
			       file line col 10))))

					;(pp pop-data)
       (setq ac-clang-project-locate-menu
	     (popup-menu* pop-data
			  :nowait t
			  :keymap map
			  :around t
			  :width (popup-preferred-width pop-data)
			  :height (min 30 (length pop-data))
			  :margin-left 1
			  :scroll-bar t 
			  :symbol t))
     
       (unwind-protect 
	   (progn (popup-draw ac-clang-project-locate-menu)
		  (setq pop (popup-menu-event-loop
			     ac-clang-project-locate-menu
			     map
			     'popup-menu-fallback)))

	 (popup-delete ac-clang-project-locate-menu)
	 ;(goto-char pt)
	 )
       pop))))

(defun ac-clang-project-locate-display-results (proc)
  (with-current-buffer (process-buffer proc)
    (let ((result)
	  (regexp (rx "PRJ_LOCATE:" (zero-or-more not-newline) ?\n
		      "desc:" (group (one-or-more (not (any ?\n)))) ?\n
		      "file:" (group (one-or-more (not (any ?\n)))) ?\n
		      "line:" (group (one-or-more (not (any ?\n)))) ?\n
		      "column:" (group (one-or-more (not (any ?\n)))) ?\n
		      "definition:" (group (one-or-more (not (any ?\n)))))))
      (goto-char (point-min))
      (while (re-search-forward regexp nil t)
	(setq result
	      (append
	       result
	       (list 
		(list (match-string 1)
		      (match-string 2)
		      (string-to-number (match-string 3))
		      (string-to-number (match-string 4))
		      (match-string 5))))))

      (when result
       (setq result 
	     (sort result #'(lambda (a b)
			      (if (string= (nth 4 a) "true")
				  t
				nil))))

       ;(pp (delete-dups result))
       (ac-clang-project-locate-popup (delete-dups result))))))


(defun ac-clang-filter-output (proc string)
  (ac-clang-append-process-output-to-process-buffer proc string)
  (if (string= (substring string -1 nil) "$")
      (case ac-clang-status
        (preempted
         (setq ac-clang-status 'idle)
         (ac-start)
         (ac-update))
        
	(locate 
	 (setq ac-clang-status 'idle)
	 (let ((result (ac-clang-parse-location-results proc)))
	   (ac-clang-goto-definition (car result) 
				     (cadr result)
				     (caddr result))))

	(prj-id 
	 (setq ac-clang-status 'idle)
	 (ac-clang-project-new-parse-result proc))

	(prj-locate 
	 (setq ac-clang-status 'idle)
	 (let ((oldpt (point))
	       (result (ac-clang-project-locate-display-results proc)))
	   (goto-char oldpt)
	   (when result
	     (let ((file (nth 0 result))
		   (line (nth 1 result))
		   (col  (nth 2 result)))
	       (ac-clang-goto-definition file line col)))))

	(prj-srcs 
	 (let ((next-src (pop ac-clang-project-srcs-queue)))
	   (if next-src 
	       (ac-clang-project-add-src proc next-src)
	     (setq ac-clang-status 'idle))))

	(prj-ignore 
	 (setq ac-clang-status 'idle))

        (otherwise
         (setq ac-clang-current-candidate
	       (ac-clang-parse-completion-results proc))
	 (message "ac-clang results arrived")
         (setq ac-clang-status 'acknowledged)
	 (ac-start :force-init t)
	 (ac-update t)
	 ;; (setq ac-prefix (buffer-substring-no-properties 
	 ;; 		  (or (ac-clang-prefix)
	 ;; 		      (point))
	 ;; 		  (point)))

	 ;; (ac-candidates)
	 ;; (ac-update)

;	 (ac-complete-clang-async)
         (setq ac-clang-status 'idle)))))

(defun ac-clang-candidate ()
  (case ac-clang-status
    (idle
     ;; (message "ac-clang-candidate triggered - fetching candidates...")

     (setq ac-clang-saved-prefix ac-prefix)
     ;; NOTE: although auto-complete would filter the result for us, but when there's
     ;;       a HUGE number of candidates avaliable it would cause auto-complete to
     ;;       block. So we filter it uncompletely here, then let auto-complete filter
     ;;       the rest later, this would ease the feeling of being "stalled" at some degree.

     ;; (message "saved prefix: %s" ac-clang-saved-prefix)
     (with-current-buffer (process-buffer ac-clang-completion-process)
       (erase-buffer))
     (setq ac-clang-status 'wait)
     (setq ac-clang-current-candidate nil)

     ;; send completion request
     (ac-clang-send-completion-request ac-clang-completion-process)
     ac-clang-current-candidate)

    (wait
     (message "ac-clang-candidate triggered - wait")
     ac-clang-current-candidate)

    (acknowledged
     ;; (message "ac-clang-candidate triggered - ack")
     (setq ac-clang-status 'idle)
     ac-clang-current-candidate)

    (preempted
     (message "clang-async is preempted by a critical request")
     nil)

    (otherwise
     (message "WTF state are we in?!"))))


;; Syntax checking with flymake

(defun ac-clang-flymake-process-sentinel ()
  (interactive)
  (setq flymake-err-info flymake-new-err-info)
  (setq flymake-new-err-info nil)
  (setq flymake-err-info
        (flymake-fix-line-numbers
         flymake-err-info 1 (flymake-count-lines)))
  (flymake-delete-own-overlays)
  (flymake-highlight-err-lines flymake-err-info))

(defun ac-clang-flymake-process-filter (process output)
  (ac-clang-append-process-output-to-process-buffer process output)
  (flymake-log 3 "received %d byte(s) of output from process %d"
               (length output) (process-id process))
  (flymake-parse-output-and-residual output)
  (when (string= (substring output -1 nil) "$")
    (flymake-parse-residual)
    (ac-clang-flymake-process-sentinel)
    (setq ac-clang-status 'idle)
    (set-process-filter ac-clang-completion-process 'ac-clang-filter-output)))

(defun ac-clang-syntax-check ()
  (interactive)
  (when (eq ac-clang-status 'idle)
    (with-current-buffer (process-buffer ac-clang-completion-process)
      (erase-buffer))
    (setq ac-clang-status 'wait)
    (set-process-filter ac-clang-completion-process 'ac-clang-flymake-process-filter)
    (ac-clang-send-syntaxcheck-request ac-clang-completion-process)))

(defvar ac-clang-location-ring (make-ring 20))

(defun ac-clang-goto-definition (file line col)
  "Similar in principle to `semantic-goto-definition'.  Invoked
by `ac-clang-parse-location-results' and will jump to FILE at
LINE and COL if it exists, storing the current location in
`ac-clang-location-ring'."
  (interactive "d")
  (cond ((file-exists-p file)
	 (condition-case err
	     (progn
	       (ring-insert ac-clang-location-ring (point-marker))
	       (message (format "Jumping to %s:%d:%d" file line col))
	       (let* ((cur-ac-clang-cflags ac-clang-cflags)
		      (cur-ac-clang-project-id ac-clang-project-id)
		      (update-cflags nil)
		      (newbuf (or (get-file-buffer file)
				  (progn 
				    (setq update-cflags t)
				    (find-file-noselect file)))))

		 (switch-to-buffer newbuf)
		 (goto-char (point-min))
		 (forward-line (1- line))
		 (forward-char (1- col))
		 (recenter)
		 (setq ac-clang-project-id cur-ac-clang-project-id)
		 (when update-cflags
		   (setq ac-clang-cflags cur-ac-clang-cflags)
		   (ac-clang-update-cmdlineargs))))
	   (error
	    ;;if not found remove the tag saved in the ring  
	    (set-marker (ring-remove ac-clang-location-ring 0) nil nil)
	    (signal (car err) (cdr err)))))
	(t
	 (message "Clang LOCATE Failure!"))))

(defun ac-clang-goto-last-location ()             
  "Pop and jump back to the last location stored by
`ac-clang-goto-definition'."
  (interactive)                                                    
  (if (ring-empty-p ac-clang-location-ring)                   
      (message "%s" "No more tags available")                      
    (let* ((marker (ring-remove ac-clang-location-ring 0))    
              (buff (marker-buffer marker))                        
                 (pos (marker-position marker)))                   
      (if (not buff)                                               
            (message "Buffer has been deleted")                    
        (switch-to-buffer buff)                                    
        (goto-char pos))                                           
      (set-marker marker nil nil))))

(defun ac-clang-shutdown-process ()
  (if ac-clang-completion-process
      (ac-clang-send-shutdown-command ac-clang-completion-process)))

(defun ac-clang-reparse-buffer ()
  (if ac-clang-completion-process
      (ac-clang-send-reparse-request ac-clang-completion-process)))

(defun ac-clang-async-preemptive ()
  (interactive)
  (self-insert-command 1)
  (if (eq ac-clang-status 'idle)
      (ac-start)
    (setq ac-clang-status 'preempted)))

(defun ac-clang-launch-completion-process ()
   (interactive)
   (if ac-clang-completion-process
       (ac-clang-filechanged)
       (ac-clang-launch-completion-process-internal)))

(defun ac-clang-relaunch-completion-process ()
   (interactive)
   (if ac-clang-completion-process
       (when (process-live-p ac-clang-completion-process)
	 (quit-process ac-clang-completion-process)))
   (ac-clang-launch-completion-process-internal))

(defun ac-clang-launch-completion-process-internal ()
   (interactive)
  (setq ac-clang-completion-process
        (let ((process-connection-type nil))
          (apply 'start-process
                 "clang-complete" "*clang-complete*"
                 ac-clang-complete-executable
                 (append (ac-clang-build-complete-args)
                         (list (buffer-file-name)))
)))

  (set-process-filter ac-clang-completion-process 'ac-clang-filter-output)
  (set-process-query-on-exit-flag ac-clang-completion-process nil)
  ;; Pre-parse source code.
  (ac-clang-send-reparse-request ac-clang-completion-process)

;;  (add-hook 'kill-buffer-hook 'ac-clang-shutdown-process nil t)
  (add-hook 'kill-emacs-hook 'ac-clang-shutdown-process nil t)
  (add-hook 'before-save-hook 'ac-clang-reparse-buffer)

  (local-set-key (kbd ".") 'ac-clang-async-preemptive)
  (local-set-key (kbd ":") 'ac-clang-async-preemptive)
  (local-set-key (kbd ">") 'ac-clang-async-preemptive))


(ac-define-source clang-async
  '((candidates . ac-clang-candidate)
    (candidate-face . ac-clang-candidate-face)
    (selection-face . ac-clang-selection-face)
    (prefix . ac-clang-prefix)
    (requires . 0)
    (document . ac-clang-document)
    (action . ac-clang-action)
    (cache)
    (symbol . "c")))

;;; auto-complete-clang-async.el ends here
