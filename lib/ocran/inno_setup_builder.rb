require "tempfile"
require_relative "windows_command_escaping"
require_relative "inno_setup_script_builder"
require_relative "launcher_batch_builder"

module Ocran
  class InnoSetupBuilder
    include WindowsCommandEscaping

    def initialize(path, inno_setup_script, chdir_before: nil, icon_path: nil, title: nil)
      @path = path
      @iss = InnoSetupScriptBuilder.new(inno_setup_script)
      @launcher = LauncherBatchBuilder.new(chdir_before: chdir_before, title: title)

      if icon_path
        cp(icon_path, File.basename(icon_path))
      end

      yield(self)

      @launcher.build
      Ocran.verbose_msg "### Application launcher batch file ###"
      Ocran.verbose_msg File.read(@launcher)

      cp(@launcher.to_path, "launcher.bat")

      @iss.build
      Ocran.verbose_msg "### INNO SETUP SCRIPT ###"
      Ocran.verbose_msg File.read(@iss)
    end

    def compile(...) = @iss.compile(...)

    def mkdir(target)
      @iss.mkdir(target)
    end

    def cp(source, target)
      @iss.cp(source, target)
    end

    def exec(image, script, *argv)
      @launcher.exec(image, script, *argv)
    end

    def export(name, value)
      @launcher.export(name, value)
    end
  end
end
