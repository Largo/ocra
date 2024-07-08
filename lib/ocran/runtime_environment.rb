# frozen_string_literal: true

module Ocran
  class RuntimeEnvironment
    class << self
      alias save new
    end

    attr_reader :env, :load_path, :pwd

    def initialize
      @env = ENV.to_hash.freeze
      @load_path = $LOAD_PATH.dup.freeze
      @pwd = Dir.pwd.freeze
    end

    # Expands the given path using the working directory stored in this
    # instance as the base. This method resolves relative paths to
    # absolute paths, ensuring they are fully qualified based on the
    # working directory stored within this instance.
    def expand_path(path)
      File.expand_path(path, @pwd)
    end
  end
end