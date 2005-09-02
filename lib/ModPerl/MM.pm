# Copyright 2000-2005 The Apache Software Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
package ModPerl::MM;

use strict;
use warnings;

use ExtUtils::MakeMaker ();
use ExtUtils::Install ();

use Cwd ();
use Carp;

our %PM; #add files to installation

# MM methods that this package overrides
no strict 'refs';
my $stash = \%{__PACKAGE__ . '::MY::'};
my @methods = grep *{$stash->{$_}}{CODE}, keys %$stash;
my $eu_mm_mv_all_methods_overriden = 0;

use strict 'refs';

sub override_eu_mm_mv_all_methods {
    my @methods = @_;

    my $orig_sub = \&ExtUtils::MakeMaker::mv_all_methods;
    no warnings 'redefine';
    *ExtUtils::MakeMaker::mv_all_methods = sub {
        # do the normal move
        $orig_sub->(@_);
        # for all the overloaded methods mv_all_method installs a stab
        # eval "package MY; sub $method { shift->SUPER::$method(\@_); }";
        # therefore we undefine our methods so on the recursive invocation of
        # Makefile.PL they will be undef, unless defined in Makefile.PL
        # and my_import will override these methods properly
        for my $sym (@methods) {
            my $name = "MY::$sym";
            undef &$name if defined &$name;
        }
    };
}

sub add_dep {
    my ($string, $targ, $add) = @_;
    $$string =~ s/($targ\s+::)/$1 $add/;
}

sub add_dep_before {
    my ($string, $targ, $before_targ, $add) = @_;
    $$string =~ s/($targ\s+::.*?) ($before_targ)/$1 $add $2/;
}

sub add_dep_after {
    my ($string, $targ, $after_targ, $add) = @_;
    $$string =~ s/($targ\s+::.*?$after_targ)/$1 $add/;
}

sub build_config {
    my $key = shift;
    require Apache2::Build;
    my $build = Apache2::Build->build_config;
    return $build unless $key;
    $build->{$key};
}

#the parent WriteMakefile moves MY:: methods into a different class
#so alias them each time WriteMakefile is called in a subdir

sub my_import {
    my $package = shift;
    no strict 'refs';
    my $stash = \%{$package . '::MY::'};
    for my $sym (keys %$stash) {
        next unless *{$stash->{$sym}}{CODE};
        my $name = "MY::$sym";
        # the method is defined in Makefile.PL
        next if defined &$name;
        # do the override behind the scenes
        *$name = *{$stash->{$sym}}{CODE};
    }
}

my @default_opts = qw(CCFLAGS LIBS INC OPTIMIZE LDDLFLAGS TYPEMAPS);
my @default_dlib_opts = qw(OTHERLDFLAGS);
my @default_macro_opts = ();
my $b = build_config();
my %opts = (
    CCFLAGS      => sub { $b->{MODPERL_CCOPTS}                        },
    LIBS         => sub { join ' ', $b->apache_libs, $b->modperl_libs },
    INC          => sub { $b->inc;                                    },
    OPTIMIZE     => sub { $b->perl_config('optimize');                },
    LDDLFLAGS    => sub { $b->perl_config('lddlflags');               },
    TYPEMAPS     => sub { $b->typemaps;                               },
    OTHERLDFLAGS => sub { $b->otherldflags;                           },
);

sub get_def_opt {
    my $opt = shift;
    return $opts{$opt}->() if exists $opts{$opt};
    # handle cases when Makefile.PL wants an option we don't have a
    # default for. XXX: some options expect [] rather than scalar.
    Carp::carp("!!! no default argument defined for argument: $opt");
    return '';
}

sub WriteMakefile {
    my %args = @_;

    # override ExtUtils::MakeMaker::mv_all_methods
    # can't do that on loading since ModPerl::MM is also use()'d
    # by ModPerl::BuildMM which itself overrides it
    unless ($eu_mm_mv_all_methods_overriden) {
        override_eu_mm_mv_all_methods(@methods);
        $eu_mm_mv_all_methods_overriden++;
    }

    my $build = build_config();
    my_import(__PACKAGE__);

    # set top-level WriteMakefile's values if weren't set already
    for (@default_opts) {
        $args{$_} = get_def_opt($_) unless exists $args{$_}; # already defined
    }

    # set dynamic_lib-level WriteMakefile's values if weren't set already
    $args{dynamic_lib} ||= {};
    my $dlib = $args{dynamic_lib};
    for (@default_dlib_opts) {
        $dlib->{$_} = get_def_opt($_) unless exists $dlib->{$_};
    }

    # set macro-level WriteMakefile's values if weren't set already
    $args{macro} ||= {};
    my $macro = $args{macro};
    for (@default_macro_opts) {
        $macro->{$_} = get_def_opt($_) unless exists $macro->{$_};
    }

    ExtUtils::MakeMaker::WriteMakefile(%args);
}

#### MM overrides ####

sub ModPerl::MM::MY::post_initialize {
    my $self = shift;

    my $build = build_config();
    my $pm = $self->{PM};

    while (my ($k, $v) = each %PM) {
        if (-e $k) {
            $pm->{$k} = $v;
        }
    }

    '';
}

1;
