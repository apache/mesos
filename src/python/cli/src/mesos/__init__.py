# See http://peak.telecommunity.com/DevCenter/setuptools#namespace-packages
# Because python does not normally allow the contents of a package to be
# retrieved from more than one location, this code snippet ensures that the
# namespace package machinery is operating and that the current package is
# registered as a namespace package.
try:
    __import__('pkg_resources').declare_namespace(__name__)
except ImportError:
    from pkgutil import extend_path
    __path__ = extend_path(__path__, __name__)
